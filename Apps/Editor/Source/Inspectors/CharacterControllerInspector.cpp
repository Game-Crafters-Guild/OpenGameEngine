#include "Inspectors/CharacterControllerInspector.h"

#include "InspectorRegistry.h"
#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/PhysicsInspectorShared.h"

#include "ECS/ECSTemplates.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "PhysicsECS/Components/CharacterController.h"

#include "UndoRedo/UndoRedoService.h"
#include "EditorChangeNotifications.h"

#include "UI/UIManager.h"

#include <cmath>
#include <cstring>

namespace GameEngine
{
namespace
{
using Editor::UndoRedoService;

static void MarkCharacterNeedsRebuild(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return;
    if (auto* c = world->GetComponentForWrite<Components::CharacterController>(entity))
        c->initialized = false;
}

struct CharacterControllerAuthoringSnapshot
{
    Physics::CharacterMotor motor{};
    float32 radius{};
    float32 height{};
    float32 maxSlopeAngleDegrees{};
    float32 maxStepHeight{};
    float32 skinWidth{};
    float32 mass{};
    float32 gravityScale{};
};

static CharacterControllerAuthoringSnapshot CaptureAuthoring(const Components::CharacterController& c)
{
    CharacterControllerAuthoringSnapshot s{};
    s.motor = c.motor;
    s.radius = c.radius;
    s.height = c.height;
    s.maxSlopeAngleDegrees = c.maxSlopeAngleDegrees;
    s.maxStepHeight = c.maxStepHeight;
    s.skinWidth = c.skinWidth;
    s.mass = c.mass;
    s.gravityScale = c.gravityScale;
    return s;
}

static void ApplyAuthoring(Components::CharacterController& c, const CharacterControllerAuthoringSnapshot& s)
{
    c.motor = s.motor;
    c.radius = s.radius;
    c.height = s.height;
    c.maxSlopeAngleDegrees = s.maxSlopeAngleDegrees;
    c.maxStepHeight = s.maxStepHeight;
    c.skinWidth = s.skinWidth;
    c.mass = s.mass;
    c.gravityScale = s.gravityScale;
}

class CharacterControllerInspectorSection final : public UIElement
{
  public:
    explicit CharacterControllerInspectorSection(const InspectorContext& ctx)
        : m_World(ctx.World),
          m_Entity(ctx.Entity),
          m_Undo(ctx.Undo),
          m_Changes(ctx.ChangeNotifications)
    {
        for (auto& ent : ctx.Entities)
            if (ent != m_Entity)
                m_AdditionalEntities.push_back(ent);
        if (!m_World || !m_Entity.IsValid())
            return;

        auto* c = m_World->GetComponent<Components::CharacterController>(m_Entity);
        if (!c)
            return;


        {
            UIElement* row = InspectorPhysicsUI::AddRow(this);
            InspectorPhysicsUI::AddLabel(
                row,
                "Motor",
                "0 = Kinematic (slides/steps; not in the physics broadphase, so raycasts miss). 1 = Dynamic (rigid body, hittable, can be pushed).");
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            m_Motor = InspectorPhysicsUI::AddInt(field, static_cast<int>(c->motor));
            m_Motor->SetOnValueChanging([this](int v) { OnMotorChanging(v); });
            m_Motor->SetOnValueChanged([this](int v) { OnMotorChanged(v); });
        }

        AddFloatRow("Radius", c->radius, m_Radius, [this](float v) { OnRadiusChanging(v); },
                    [this](float v) { OnRadiusChanged(v); }, "Capsule radius in meters");
        AddFloatRow("Height", c->height, m_Height, [this](float v) { OnHeightChanging(v); },
                    [this](float v) { OnHeightChanged(v); }, "Total capsule height including caps");
        AddFloatRow("Max Slope", c->maxSlopeAngleDegrees, m_MaxSlope, [this](float v) { OnMaxSlopeChanging(v); },
                    [this](float v) { OnMaxSlopeChanged(v); }, "Steepest walkable slope in degrees");
        AddFloatRow("Step Height", c->maxStepHeight, m_MaxStep, [this](float v) { OnMaxStepChanging(v); },
                    [this](float v) { OnMaxStepChanged(v); }, "Maximum step-up height in meters");
        AddFloatRow("Skin Width", c->skinWidth, m_Skin, [this](float v) { OnSkinChanging(v); },
                    [this](float v) { OnSkinChanged(v); }, "Kinematic contact padding");
        AddFloatRow("Mass", c->mass, m_Mass, [this](float v) { OnMassChanging(v); },
                    [this](float v) { OnMassChanged(v); }, "Used by Dynamic motor");
        AddFloatRow("Gravity Scale", c->gravityScale, m_GravityScale, [this](float v) { OnGravityChanging(v); },
                    [this](float v) { OnGravityChanged(v); }, "Gravity multiplier (1 = world gravity)");

        if (ctx.SimulationRefreshCallbacks)
            ctx.SimulationRefreshCallbacks->push_back([this]() { RefreshFromWorld(); });
    }

    ~CharacterControllerInspectorSection() override
    {
        if (m_ActiveEdit)
            m_ActiveEdit.Commit();
    }

  private:
    void RefreshFromWorld()
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        auto* c = m_World->GetComponent<Components::CharacterController>(m_Entity);
        if (!c)
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

        setInt(m_Motor, static_cast<int>(c->motor));
        setFloat(m_Radius, c->radius);
        setFloat(m_Height, c->height);
        setFloat(m_MaxSlope, c->maxSlopeAngleDegrees);
        setFloat(m_MaxStep, c->maxStepHeight);
        setFloat(m_Skin, c->skinWidth);
        setFloat(m_Mass, c->mass);
        setFloat(m_GravityScale, c->gravityScale);
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

    UndoRedoService::SnapshotTarget MakeSnapshotTarget()
    {
        std::vector<ECS::EntityHandle> allEntities;
        allEntities.push_back(m_Entity);
        for (auto& ex : m_AdditionalEntities)
            allEntities.push_back(ex);

        UndoRedoService::SnapshotTarget t{};
        t.debugLabel = "CharacterController";
        t.Capture = [this, allEntities](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
        {
            constexpr std::size_t kSize = sizeof(CharacterControllerAuthoringSnapshot);
            out.resize(allEntities.size() * kSize);
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto* c = m_World->GetComponent<Components::CharacterController>(allEntities[i]);
                if (!c)
                    return false;
                const CharacterControllerAuthoringSnapshot s = CaptureAuthoring(*c);
                std::memcpy(out.data() + i * kSize, &s, kSize);
            }
            return true;
        };
        t.Apply = [this, allEntities](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
        {
            constexpr std::size_t kSize = sizeof(CharacterControllerAuthoringSnapshot);
            if (in.size() != allEntities.size() * kSize)
                return false;
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto ent = allEntities[i];
                if (!m_World || !ent.IsValid() || !m_World->IsValid(ent))
                    continue;
                auto* c = m_World->GetComponentForWrite<Components::CharacterController>(ent);
                if (!c)
                    continue;
                CharacterControllerAuthoringSnapshot s{};
                std::memcpy(&s, in.data() + i * kSize, kSize);
                ApplyAuthoring(*c, s);
                MarkCharacterNeedsRebuild(m_World, ent);
            }
            return true;
        };
        t.Notify = [this, allEntities](Editor::EditorChangeNotifications::ChangeKind kind)
        {
            if (!m_Changes)
                return;
            for (auto& ent : allEntities)
                m_Changes->NotifyComponentChange<Components::CharacterController>(m_World, ent, kind);
        };
        return t;
    }

    template <typename ApplyFn>
    void PreviewEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkCharacterNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::CharacterController>(
                    m_World, m_Entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
            BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Preview);
            return;
        }
        if (!m_ActiveEdit)
            m_ActiveEdit = m_Undo->BeginInteractiveEdit(name, MakeSnapshotTarget());
        m_ActiveEdit.Preview([&]()
                             {
                                 applyFn();
                                 MarkCharacterNeedsRebuild(m_World, m_Entity);
                             });
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Preview);
    }

    template <typename ApplyFn>
    void CommitEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkCharacterNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentCommit<Components::CharacterController>(m_World, m_Entity);
            BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
            return;
        }
        if (!m_ActiveEdit)
            m_ActiveEdit = m_Undo->BeginInteractiveEdit(name, MakeSnapshotTarget());
        m_ActiveEdit.Preview([&]()
                             {
                                 applyFn();
                                 MarkCharacterNeedsRebuild(m_World, m_Entity);
                             });
        m_ActiveEdit.Commit();
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
    }

    template <typename ApplyFn>
    void BroadcastToAdditional(ApplyFn&& applyFn, Editor::EditorChangeNotifications::ChangeKind kind)
    {
        ECS::EntityHandle saved = m_Entity;
        for (auto& ex : m_AdditionalEntities)
        {
            m_Entity = ex;
            applyFn();
            MarkCharacterNeedsRebuild(m_World, ex);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::CharacterController>(m_World, ex, kind);
        }
        m_Entity = saved;
    }

    static Physics::CharacterMotor ClampMotor(int v)
    {
        if (v >= 1)
            return Physics::CharacterMotor::Dynamic;
        return Physics::CharacterMotor::Kinematic;
    }

    void OnMotorChanging(int v)
    {
        PreviewEdit("Character Motor", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->motor = ClampMotor(v);
                    });
    }
    void OnMotorChanged(int v)
    {
        CommitEdit("Character Motor", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->motor = ClampMotor(v);
                   });
    }

    void OnRadiusChanging(float v)
    {
        PreviewEdit("Character Radius", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->radius = v;
                    });
    }
    void OnRadiusChanged(float v)
    {
        CommitEdit("Character Radius", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->radius = v;
                   });
    }

    void OnHeightChanging(float v)
    {
        PreviewEdit("Character Height", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->height = v;
                    });
    }
    void OnHeightChanged(float v)
    {
        CommitEdit("Character Height", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->height = v;
                   });
    }

    void OnMaxSlopeChanging(float v)
    {
        PreviewEdit("Character Max Slope", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->maxSlopeAngleDegrees = v;
                    });
    }
    void OnMaxSlopeChanged(float v)
    {
        CommitEdit("Character Max Slope", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->maxSlopeAngleDegrees = v;
                   });
    }

    void OnMaxStepChanging(float v)
    {
        PreviewEdit("Character Step Height", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->maxStepHeight = v;
                    });
    }
    void OnMaxStepChanged(float v)
    {
        CommitEdit("Character Step Height", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->maxStepHeight = v;
                   });
    }

    void OnSkinChanging(float v)
    {
        PreviewEdit("Character Skin Width", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->skinWidth = v;
                    });
    }
    void OnSkinChanged(float v)
    {
        CommitEdit("Character Skin Width", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->skinWidth = v;
                   });
    }

    void OnMassChanging(float v)
    {
        PreviewEdit("Character Mass", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->mass = v;
                    });
    }
    void OnMassChanged(float v)
    {
        CommitEdit("Character Mass", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->mass = v;
                   });
    }

    void OnGravityChanging(float v)
    {
        PreviewEdit("Character Gravity Scale", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                            c->gravityScale = v;
                    });
    }
    void OnGravityChanged(float v)
    {
        CommitEdit("Character Gravity Scale", [this, v]()
                   {
                       if (auto* c = m_World->GetComponentForWrite<Components::CharacterController>(m_Entity))
                           c->gravityScale = v;
                   });
    }

    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    std::vector<ECS::EntityHandle> m_AdditionalEntities;
    Editor::UndoRedoService* m_Undo = nullptr;
    Editor::EditorChangeNotifications* m_Changes = nullptr;
    Editor::UndoRedoService::InteractiveEdit m_ActiveEdit{};

    IntField* m_Motor = nullptr;
    FloatField* m_Radius = nullptr;
    FloatField* m_Height = nullptr;
    FloatField* m_MaxSlope = nullptr;
    FloatField* m_MaxStep = nullptr;
    FloatField* m_Skin = nullptr;
    FloatField* m_Mass = nullptr;
    FloatField* m_GravityScale = nullptr;
};

} // namespace

void RegisterCharacterControllerInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto section = std::make_unique<CharacterControllerInspectorSection>(ctx);
        ctx.Parent->AddChild(std::move(section));
    };
    InspectorRegistry::Get().RegisterComponentInspector<Components::CharacterController>(std::move(fn));

    Editor::EditorComponentTraits traits;
    traits.EnableToggleTooltip =
        "Switch this component on or off. Off removes its physics character; on rebuilds it from these values.";
    Editor::EditorComponentTraitsRegistry::Get().Register(
        ECS::GetComponentTypeId<Components::CharacterController>(), std::move(traits));
}

} // namespace GameEngine
