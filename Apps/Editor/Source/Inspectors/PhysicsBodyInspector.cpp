#include "Inspectors/PhysicsBodyInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"

#include "ECS/ECSTemplates.h" // brings in ECS::World definition (via Entity.h)
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"

#include "UndoRedo/UndoRedoService.h"
#include "EditorChangeNotifications.h"

#include "Inspectors/PhysicsInspectorShared.h"

#include "UI/UIManager.h"

#include <cmath>

namespace GameEngine
{
namespace
{
using Editor::UndoRedoService;

static void MarkPhysicsBodyNeedsRebuild(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;

    // Primary case: body lives on this entity.
    if (auto* body = world->GetComponentForWrite<Components::PhysicsBody>(entity))
    {
        body->initialized = false;
        return;
    }

    // Compound case: collider entity contributes to an owner body.
    if (auto* owner = world->GetComponent<Components::PhysicsColliderOwner>(entity))
    {
        ECS::EntityHandle bodyEntity = owner->body.IsValid() ? owner->body : entity;
        if (auto* body = world->GetComponentForWrite<Components::PhysicsBody>(bodyEntity))
        {
            body->initialized = false;
        }
    }
}

// Snapshot only the authoring portion of PhysicsBody (avoid runtime handles).
struct PhysicsBodyAuthoringSnapshot
{
    Physics::MotionType motionType{};
    float32 mass{};
    float32 linearDamping{};
    float32 angularDamping{};
    float32 gravityScale{};
    float32 centerOfMassOffsetX{};
    float32 centerOfMassOffsetY{};
    float32 centerOfMassOffsetZ{};
    Physics::PhysicsMaterial material{};
    float32 linearVelocityX{};
    float32 linearVelocityY{};
    float32 linearVelocityZ{};
    float32 angularVelocityX{};
    float32 angularVelocityY{};
    float32 angularVelocityZ{};
    bool allowSleep{};
    bool startAwake{};
    bool continuousCollision{};
};

static PhysicsBodyAuthoringSnapshot CaptureAuthoring(const Components::PhysicsBody& b)
{
    PhysicsBodyAuthoringSnapshot s{};
    s.motionType = b.motionType;
    s.mass = b.mass;
    s.linearDamping = b.linearDamping;
    s.angularDamping = b.angularDamping;
    s.gravityScale = b.gravityScale;
    s.centerOfMassOffsetX = b.centerOfMassOffsetX;
    s.centerOfMassOffsetY = b.centerOfMassOffsetY;
    s.centerOfMassOffsetZ = b.centerOfMassOffsetZ;
    s.material = b.material;
    s.linearVelocityX = b.linearVelocityX;
    s.linearVelocityY = b.linearVelocityY;
    s.linearVelocityZ = b.linearVelocityZ;
    s.angularVelocityX = b.angularVelocityX;
    s.angularVelocityY = b.angularVelocityY;
    s.angularVelocityZ = b.angularVelocityZ;
    s.allowSleep = b.allowSleep;
    s.startAwake = b.startAwake;
    s.continuousCollision = b.continuousCollision;
    return s;
}

static void ApplyAuthoring(Components::PhysicsBody& b, const PhysicsBodyAuthoringSnapshot& s)
{
    b.motionType = s.motionType;
    b.mass = s.mass;
    b.linearDamping = s.linearDamping;
    b.angularDamping = s.angularDamping;
    b.gravityScale = s.gravityScale;
    b.centerOfMassOffsetX = s.centerOfMassOffsetX;
    b.centerOfMassOffsetY = s.centerOfMassOffsetY;
    b.centerOfMassOffsetZ = s.centerOfMassOffsetZ;
    b.material = s.material;
    b.linearVelocityX = s.linearVelocityX;
    b.linearVelocityY = s.linearVelocityY;
    b.linearVelocityZ = s.linearVelocityZ;
    b.angularVelocityX = s.angularVelocityX;
    b.angularVelocityY = s.angularVelocityY;
    b.angularVelocityZ = s.angularVelocityZ;
    b.allowSleep = s.allowSleep;
    b.startAwake = s.startAwake;
    b.continuousCollision = s.continuousCollision;
}

class PhysicsBodyInspectorSection final : public UIElement
{
  public:
    explicit PhysicsBodyInspectorSection(const InspectorContext& ctx)
        : m_World(ctx.World),
          m_Entity(ctx.Entity),
          m_Undo(ctx.Undo),
          m_Changes(ctx.ChangeNotifications)
    {
        for (auto& ent : ctx.Entities)
            if (ent != m_Entity) m_AdditionalEntities.push_back(ent);
        if (!m_World || !m_Entity.IsValid())
            return;

        auto* body = m_World->GetComponent<Components::PhysicsBody>(m_Entity);
        if (!body)
            return;

        // Motion type (simple int for now: 0=Static, 1=Kinematic, 2=Dynamic)
        {
            UIElement* row = InspectorPhysicsUI::AddRow(this);
            InspectorPhysicsUI::AddLabel(row, "Motion Type", "0 = Static, 1 = Kinematic, 2 = Dynamic");
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            m_MotionType = InspectorPhysicsUI::AddInt(field, static_cast<int>(body->motionType));
            m_MotionType->SetOnValueChanging([this](int v) { OnMotionTypeChanging(v); });
            m_MotionType->SetOnValueChanged([this](int v) { OnMotionTypeChanged(v); });
        }

        // Mass / damping / gravity
        AddFloatRow("Mass", body->mass, m_Mass,
                    [this](float v) { OnMassChanging(v); },
                    [this](float v) { OnMassChanged(v); },
                    "Body mass in kilograms");

        AddFloatRow("Linear Damping", body->linearDamping, m_LinearDamping,
                    [this](float v) { OnLinearDampingChanging(v); },
                    [this](float v) { OnLinearDampingChanged(v); },
                    "Linear velocity damping coefficient (0 = no drag)");

        AddFloatRow("Angular Damping", body->angularDamping, m_AngularDamping,
                    [this](float v) { OnAngularDampingChanging(v); },
                    [this](float v) { OnAngularDampingChanged(v); },
                    "Angular velocity damping coefficient (0 = no rotational drag)");

        AddFloatRow("Gravity Scale", body->gravityScale, m_GravityScale,
                    [this](float v) { OnGravityScaleChanging(v); },
                    [this](float v) { OnGravityScaleChanged(v); },
                    "Gravity multiplier for this body (1 = normal, 0 = weightless)");

        AddFloatRow("Center Mass X", body->centerOfMassOffsetX, m_CenterOfMassOffsetX,
                    [this](float v) { OnCenterOfMassOffsetXChanging(v); },
                    [this](float v) { OnCenterOfMassOffsetXChanged(v); },
                    "Local center-of-mass offset on X");

        AddFloatRow("Center Mass Y", body->centerOfMassOffsetY, m_CenterOfMassOffsetY,
                    [this](float v) { OnCenterOfMassOffsetYChanging(v); },
                    [this](float v) { OnCenterOfMassOffsetYChanged(v); },
                    "Local center-of-mass offset on Y; negative values stabilize boats");

        AddFloatRow("Center Mass Z", body->centerOfMassOffsetZ, m_CenterOfMassOffsetZ,
                    [this](float v) { OnCenterOfMassOffsetZChanging(v); },
                    [this](float v) { OnCenterOfMassOffsetZChanged(v); },
                    "Local center-of-mass offset on Z");

        // Material
        AddFloatRow("Friction", body->material.friction, m_Friction,
                    [this](float v) { OnFrictionChanging(v); },
                    [this](float v) { OnFrictionChanged(v); },
                    "Surface friction coefficient (0 = frictionless, 1 = high friction)");

        AddFloatRow("Restitution", body->material.restitution, m_Restitution,
                    [this](float v) { OnRestitutionChanging(v); },
                    [this](float v) { OnRestitutionChanged(v); },
                    "Bounciness coefficient (0 = no bounce, 1 = fully elastic)");

        // Flags
        AddToggleRow("Allow Sleep", body->allowSleep, m_AllowSleep,
                     [this](bool v) { if (!m_RefreshingFromWorld) OnAllowSleepChanged(v); },
                     "Allow the physics engine to deactivate this body when at rest");

        AddToggleRow("Start Awake", body->startAwake, m_StartAwake,
                     [this](bool v) { if (!m_RefreshingFromWorld) OnStartAwakeChanged(v); },
                     "Start the simulation with this body active");

        AddToggleRow("Continuous CCD", body->continuousCollision, m_ContinuousCollision,
                     [this](bool v) { if (!m_RefreshingFromWorld) OnContinuousCollisionChanged(v); },
                     "Enable continuous collision detection for fast-moving bodies");

        if (ctx.SimulationRefreshCallbacks)
            ctx.SimulationRefreshCallbacks->push_back([this]() { RefreshFromWorld(); });
    }

    ~PhysicsBodyInspectorSection() override
    {
        if (m_ActiveEdit)
        {
            m_ActiveEdit.Commit();
        }
    }

  private:
    void RefreshFromWorld()
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;

        auto* body = m_World->GetComponent<Components::PhysicsBody>(m_Entity);
        if (!body)
            return;

        UIManager* ui = GetOwnerManager();
        static const std::string kEmpty;
        const std::string& focusId = ui ? ui->GetFocusedElementId() : kEmpty;

        auto setFloat = [&focusId](FloatField* field, float value)
        {
            if (!field || InspectorUI::ContainsFocusedElement(field, focusId))
                return;
            if (std::fabs(field->GetValue() - value) > 1.0e-4f)
                field->SetValue(value);
        };

        auto setInt = [&focusId](IntField* field, int value)
        {
            if (!field || InspectorUI::ContainsFocusedElement(field, focusId))
                return;
            if (field->GetValue() != value)
                field->SetValue(value);
        };

        setInt(m_MotionType, static_cast<int>(body->motionType));
        setFloat(m_Mass, body->mass);
        setFloat(m_LinearDamping, body->linearDamping);
        setFloat(m_AngularDamping, body->angularDamping);
        setFloat(m_GravityScale, body->gravityScale);
        setFloat(m_CenterOfMassOffsetX, body->centerOfMassOffsetX);
        setFloat(m_CenterOfMassOffsetY, body->centerOfMassOffsetY);
        setFloat(m_CenterOfMassOffsetZ, body->centerOfMassOffsetZ);
        setFloat(m_Friction, body->material.friction);
        setFloat(m_Restitution, body->material.restitution);

        // Guard: SetChecked fires OnValueChanged; prevent write-back during refresh.
        m_RefreshingFromWorld = true;
        if (m_AllowSleep && m_AllowSleep->IsChecked() != body->allowSleep)
            m_AllowSleep->SetChecked(body->allowSleep);
        if (m_StartAwake && m_StartAwake->IsChecked() != body->startAwake)
            m_StartAwake->SetChecked(body->startAwake);
        if (m_ContinuousCollision && m_ContinuousCollision->IsChecked() != body->continuousCollision)
            m_ContinuousCollision->SetChecked(body->continuousCollision);
        m_RefreshingFromWorld = false;
    }

    template <typename ChangingFn, typename ChangedFn>
    void AddFloatRow(const std::string& label, float initial, FloatField*& outField, ChangingFn&& onChanging, ChangedFn&& onChanged, const char* tooltip = nullptr)
    {
        UIElement* row = InspectorPhysicsUI::AddRow(this);
        InspectorPhysicsUI::AddLabel(row, label, tooltip);
        UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
        outField = InspectorPhysicsUI::AddFloat(field, initial);
        outField->SetOnValueChanging(std::forward<ChangingFn>(onChanging));
        outField->SetOnValueChanged(std::forward<ChangedFn>(onChanged));
    }

    template <typename ChangedFn>
    void AddToggleRow(const std::string& label, bool initial, Toggle*& outToggle, ChangedFn&& onChanged, const char* tooltip = nullptr)
    {
        UIElement* row = InspectorPhysicsUI::AddRow(this);
        InspectorPhysicsUI::AddLabel(row, label, tooltip);
        UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
        outToggle = InspectorPhysicsUI::AddToggle(field, initial);
        outToggle->SetOnValueChanged(std::forward<ChangedFn>(onChanged));
    }

    UndoRedoService::SnapshotTarget MakeSnapshotTarget()
    {
        // Build list of all entities to snapshot (primary + additional).
        std::vector<ECS::EntityHandle> allEntities;
        allEntities.push_back(m_Entity);
        for (auto& ex : m_AdditionalEntities)
            allEntities.push_back(ex);

        UndoRedoService::SnapshotTarget t{};
        t.debugLabel = "PhysicsBody";
        t.Capture = [this, allEntities](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
        {
            constexpr std::size_t kSize = sizeof(PhysicsBodyAuthoringSnapshot);
            out.resize(allEntities.size() * kSize);
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto* body = m_World->GetComponent<Components::PhysicsBody>(allEntities[i]);
                if (!body)
                    return false;
                const PhysicsBodyAuthoringSnapshot s = CaptureAuthoring(*body);
                std::memcpy(out.data() + i * kSize, &s, kSize);
            }
            return true;
        };
        t.Apply = [this, allEntities](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
        {
            constexpr std::size_t kSize = sizeof(PhysicsBodyAuthoringSnapshot);
            if (in.size() != allEntities.size() * kSize)
                return false;
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto ent = allEntities[i];
                if (!m_World || !ent.IsValid() || !m_World->IsValid(ent))
                    continue;
                auto* body = m_World->GetComponentForWrite<Components::PhysicsBody>(ent);
                if (!body)
                    continue;
                PhysicsBodyAuthoringSnapshot s{};
                std::memcpy(&s, in.data() + i * kSize, kSize);
                ApplyAuthoring(*body, s);
                MarkPhysicsBodyNeedsRebuild(m_World, ent);
            }
            return true;
        };
        t.Notify = [this, allEntities](Editor::EditorChangeNotifications::ChangeKind kind)
        {
            if (!m_Changes)
                return;
            for (auto& ent : allEntities)
                m_Changes->NotifyComponentChange<Components::PhysicsBody>(m_World, ent, kind);
        };
        return t;
    }

    template <typename ApplyFn>
    void PreviewEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkPhysicsBodyNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::PhysicsBody>(m_World, m_Entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
            BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Preview);
            return;
        }
        if (!m_ActiveEdit)
        {
            m_ActiveEdit = m_Undo->BeginInteractiveEdit(name, MakeSnapshotTarget());
        }
        m_ActiveEdit.Preview([&]()
                             {
                                 applyFn();
                                 MarkPhysicsBodyNeedsRebuild(m_World, m_Entity);
                             });
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Preview);
    }

    template <typename ApplyFn>
    void CommitEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkPhysicsBodyNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentCommit<Components::PhysicsBody>(m_World, m_Entity);
            BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
            return;
        }
        if (!m_ActiveEdit)
        {
            m_ActiveEdit = m_Undo->BeginInteractiveEdit(name, MakeSnapshotTarget());
        }
        // Ensure final value is applied before commit.
        m_ActiveEdit.Preview([&]()
                             {
                                 applyFn();
                                 MarkPhysicsBodyNeedsRebuild(m_World, m_Entity);
                             });
        m_ActiveEdit.Commit();
        m_ActiveEdit = {};
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
    }

    void OnMotionTypeChanging(int v)
    {
        PreviewEdit("Physics Motion Type", [this, v]()
                    {
                        if (auto* body = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity))
                        {
                            int clamped = v;
                            if (clamped < 0) clamped = 0;
                            if (clamped > 2) clamped = 2;
                            body->motionType = static_cast<Physics::MotionType>(clamped);
                        }
                    });
    }
    void OnMotionTypeChanged(int v) { CommitEdit("Physics Motion Type", [this, v]() {
        if (auto* body = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity))
        {
            int clamped = v;
            if (clamped < 0) clamped = 0;
            if (clamped > 2) clamped = 2;
            body->motionType = static_cast<Physics::MotionType>(clamped);
        }
    }); }

    void OnMassChanging(float v) { PreviewEdit("Physics Mass", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->mass = v; }); }
    void OnMassChanged(float v) { CommitEdit("Physics Mass", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->mass = v; }); }

    void OnLinearDampingChanging(float v) { PreviewEdit("Physics Linear Damping", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->linearDamping = v; }); }
    void OnLinearDampingChanged(float v) { CommitEdit("Physics Linear Damping", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->linearDamping = v; }); }

    void OnAngularDampingChanging(float v) { PreviewEdit("Physics Angular Damping", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->angularDamping = v; }); }
    void OnAngularDampingChanged(float v) { CommitEdit("Physics Angular Damping", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->angularDamping = v; }); }

    void OnGravityScaleChanging(float v) { PreviewEdit("Physics Gravity Scale", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->gravityScale = v; }); }
    void OnGravityScaleChanged(float v) { CommitEdit("Physics Gravity Scale", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->gravityScale = v; }); }

    void OnCenterOfMassOffsetXChanging(float v) { PreviewEdit("Physics Center Mass X", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetX = v; }); }
    void OnCenterOfMassOffsetXChanged(float v) { CommitEdit("Physics Center Mass X", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetX = v; }); }

    void OnCenterOfMassOffsetYChanging(float v) { PreviewEdit("Physics Center Mass Y", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetY = v; }); }
    void OnCenterOfMassOffsetYChanged(float v) { CommitEdit("Physics Center Mass Y", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetY = v; }); }

    void OnCenterOfMassOffsetZChanging(float v) { PreviewEdit("Physics Center Mass Z", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetZ = v; }); }
    void OnCenterOfMassOffsetZChanged(float v) { CommitEdit("Physics Center Mass Z", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->centerOfMassOffsetZ = v; }); }

    void OnFrictionChanging(float v) { PreviewEdit("Physics Friction", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->material.friction = v; }); }
    void OnFrictionChanged(float v) { CommitEdit("Physics Friction", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->material.friction = v; }); }

    void OnRestitutionChanging(float v) { PreviewEdit("Physics Restitution", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->material.restitution = v; }); }
    void OnRestitutionChanged(float v) { CommitEdit("Physics Restitution", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->material.restitution = v; }); }

    void OnAllowSleepChanged(bool v) { CommitEdit("Physics Allow Sleep", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->allowSleep = v; }); }
    void OnStartAwakeChanged(bool v) { CommitEdit("Physics Start Awake", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->startAwake = v; }); }
    void OnContinuousCollisionChanged(bool v) { CommitEdit("Physics Continuous Collision", [this, v]() { if (auto* b = m_World->GetComponentForWrite<Components::PhysicsBody>(m_Entity)) b->continuousCollision = v; }); }

    template <typename ApplyFn>
    void BroadcastToAdditional(ApplyFn&& applyFn, Editor::EditorChangeNotifications::ChangeKind kind)
    {
        ECS::EntityHandle saved = m_Entity;
        for (auto& ex : m_AdditionalEntities)
        {
            m_Entity = ex;
            applyFn();
            MarkPhysicsBodyNeedsRebuild(m_World, ex);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::PhysicsBody>(m_World, ex, kind);
        }
        m_Entity = saved;
    }

  private:
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    std::vector<ECS::EntityHandle> m_AdditionalEntities;
    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::EditorChangeNotifications* m_Changes = nullptr;

    Editor::UndoRedoService::InteractiveEdit m_ActiveEdit{};

    IntField* m_MotionType = nullptr;
    FloatField* m_Mass = nullptr;
    FloatField* m_LinearDamping = nullptr;
    FloatField* m_AngularDamping = nullptr;
    FloatField* m_GravityScale = nullptr;
    FloatField* m_CenterOfMassOffsetX = nullptr;
    FloatField* m_CenterOfMassOffsetY = nullptr;
    FloatField* m_CenterOfMassOffsetZ = nullptr;
    FloatField* m_Friction = nullptr;
    FloatField* m_Restitution = nullptr;
    Toggle* m_AllowSleep = nullptr;
    Toggle* m_StartAwake = nullptr;
    Toggle* m_ContinuousCollision = nullptr;
    bool m_RefreshingFromWorld = false;
};

} // namespace

void RegisterPhysicsBodyInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;

        auto section = std::make_unique<PhysicsBodyInspectorSection>(ctx);
        ctx.Parent->AddChild(std::move(section));
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::PhysicsBody>(std::move(fn));
}

} // namespace GameEngine
