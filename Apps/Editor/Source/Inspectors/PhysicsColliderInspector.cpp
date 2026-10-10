#include "Inspectors/PhysicsColliderInspector.h"

#include "InspectorRegistry.h"

#include <functional>
#include <memory>

#include "ECS/ECSTemplates.h" // brings in ECS::World definition (via Entity.h)
#include "PhysicsECS/Components/PhysicsBody.h"
#include "PhysicsECS/Components/PhysicsCollider.h"
#include "PhysicsECS/Components/PhysicsColliderOwner.h"
#include "PhysicsECS/Components/BoxColliderShape.h"
#include "PhysicsECS/Components/SphereColliderShape.h"
#include "PhysicsECS/Components/CapsuleColliderShape.h"
#include "PhysicsECS/Components/PlaneColliderShape.h"

#include "UndoRedo/UndoRedoService.h"
#include "EditorChangeNotifications.h"

#include "Inspectors/InspectorUIHelpers.h"
#include "Inspectors/PhysicsInspectorShared.h"

namespace GameEngine
{
namespace
{
using Editor::UndoRedoService;

static ECS::EntityHandle ResolveOwningBody(ECS::World* world, ECS::EntityHandle entity)
{
    if (!world || !entity.IsValid() || !world->IsValid(entity))
        return {};
    if (world->HasComponent<Components::PhysicsBody>(entity))
        return entity;
    if (auto* owner = world->GetComponent<Components::PhysicsColliderOwner>(entity))
    {
        if (owner->body.IsValid() && world->IsValid(owner->body))
            return owner->body;
    }
    return entity;
}

static void MarkOwningBodyNeedsRebuild(ECS::World* world, ECS::EntityHandle entity)
{
    ECS::EntityHandle bodyEntity = ResolveOwningBody(world, entity);
    if (!bodyEntity.IsValid())
        return;
    if (auto* body = world->GetComponentForWrite<Components::PhysicsBody>(bodyEntity))
    {
        body->initialized = false;
    }
}

// --- PhysicsCollider -------------------------------------------------------
struct PhysicsColliderSnapshot
{
    Physics::CollisionLayer layer{};
    Physics::CollisionGroup collisionGroup{};
    uint32 belongsToMask{};
    uint32 collidesWithMask{};
    bool isTrigger{};
    bool overrideMaterial{};
    Physics::PhysicsMaterial material{};
};

static PhysicsColliderSnapshot CaptureCollider(const Components::PhysicsCollider& c)
{
    PhysicsColliderSnapshot s{};
    s.layer = c.layer;
    s.collisionGroup = c.collisionGroup;
    s.belongsToMask = c.belongsToMask;
    s.collidesWithMask = c.collidesWithMask;
    s.isTrigger = c.isTrigger;
    s.overrideMaterial = c.overrideMaterial;
    s.material = c.material;
    return s;
}

static void ApplyCollider(Components::PhysicsCollider& c, const PhysicsColliderSnapshot& s)
{
    c.layer = s.layer;
    c.collisionGroup = s.collisionGroup;
    c.belongsToMask = s.belongsToMask;
    c.collidesWithMask = s.collidesWithMask;
    c.isTrigger = s.isTrigger;
    c.overrideMaterial = s.overrideMaterial;
    c.material = s.material;
}

class PhysicsColliderInspectorSection final : public UIElement
{
  public:
    explicit PhysicsColliderInspectorSection(const InspectorContext& ctx)
        : m_World(ctx.World),
          m_Entity(ctx.Entity),
          m_Undo(ctx.Undo),
          m_Changes(ctx.ChangeNotifications)
    {
        for (auto& ent : ctx.Entities)
            if (ent != m_Entity) m_AdditionalEntities.push_back(ent);
        if (!m_World || !m_Entity.IsValid())
            return;

        auto* col = m_World->GetComponent<Components::PhysicsCollider>(m_Entity);
        if (!col)
            return;

        {
            UIElement* row = InspectorPhysicsUI::AddRow(this);
            InspectorPhysicsUI::AddLabel(row, "Layer", "Physics collision layer for filtering contact pairs");
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            m_Layer = InspectorPhysicsUI::AddInt(field, static_cast<int>(col->layer));
            m_Layer->SetOnValueChanging([this](int v) { OnLayerChanging(v); });
            m_Layer->SetOnValueChanged([this](int v) { OnLayerChanged(v); });
        }

        {
            UIElement* row = InspectorPhysicsUI::AddRow(this);
            InspectorPhysicsUI::AddLabel(row, "Trigger", "When enabled, collider detects overlaps but produces no physical response");
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            m_IsTrigger = InspectorPhysicsUI::AddToggle(field, col->isTrigger);
            m_IsTrigger->SetOnValueChanged([this](bool v) { OnIsTriggerChanged(v); });
        }

        {
            UIElement* row = InspectorPhysicsUI::AddRow(this);
            InspectorPhysicsUI::AddLabel(row, "Override Material", "Use per-collider friction and restitution instead of the body's material");
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            m_OverrideMaterial = InspectorPhysicsUI::AddToggle(field, col->overrideMaterial);
            m_OverrideMaterial->SetOnValueChanged([this](bool v) { OnOverrideMaterialChanged(v); });
        }

        AddFloatRow("Friction", col->material.friction, m_Friction,
                    [this](float v) { OnFrictionChanging(v); },
                    [this](float v) { OnFrictionChanged(v); },
                    "Surface friction coefficient (0 = frictionless, 1 = high friction)");

        AddFloatRow("Restitution", col->material.restitution, m_Restitution,
                    [this](float v) { OnRestitutionChanging(v); },
                    [this](float v) { OnRestitutionChanged(v); },
                    "Bounciness coefficient (0 = no bounce, 1 = fully elastic)");
    }

    ~PhysicsColliderInspectorSection() override
    {
        if (m_ActiveEdit)
        {
            m_ActiveEdit.Commit();
        }
    }

  private:
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
        t.debugLabel = "PhysicsCollider";
        t.Capture = [this, allEntities](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
        {
            constexpr std::size_t kSize = sizeof(PhysicsColliderSnapshot);
            out.resize(allEntities.size() * kSize);
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto* col = m_World->GetComponent<Components::PhysicsCollider>(allEntities[i]);
                if (!col)
                    return false;
                const PhysicsColliderSnapshot s = CaptureCollider(*col);
                std::memcpy(out.data() + i * kSize, &s, kSize);
            }
            return true;
        };
        t.Apply = [this, allEntities](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
        {
            constexpr std::size_t kSize = sizeof(PhysicsColliderSnapshot);
            if (in.size() != allEntities.size() * kSize)
                return false;
            for (std::size_t i = 0; i < allEntities.size(); ++i)
            {
                auto ent = allEntities[i];
                if (!m_World || !ent.IsValid() || !m_World->IsValid(ent))
                    continue;
                auto* col = m_World->GetComponentForWrite<Components::PhysicsCollider>(ent);
                if (!col)
                    continue;
                PhysicsColliderSnapshot s{};
                std::memcpy(&s, in.data() + i * kSize, kSize);
                ApplyCollider(*col, s);
                MarkOwningBodyNeedsRebuild(m_World, ent);
            }
            return true;
        };
        t.Notify = [this, allEntities](Editor::EditorChangeNotifications::ChangeKind kind)
        {
            if (!m_Changes)
                return;
            for (auto& ent : allEntities)
                m_Changes->NotifyComponentChange<Components::PhysicsCollider>(m_World, ent, kind);
        };
        return t;
    }

    template <typename ApplyFn>
    void PreviewEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkOwningBodyNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::PhysicsCollider>(m_World, m_Entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
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
                                 MarkOwningBodyNeedsRebuild(m_World, m_Entity);
                             });
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Preview);
    }

    template <typename ApplyFn>
    void CommitEdit(const char* name, ApplyFn&& applyFn)
    {
        if (!m_Undo)
        {
            applyFn();
            MarkOwningBodyNeedsRebuild(m_World, m_Entity);
            if (m_Changes)
                m_Changes->NotifyComponentCommit<Components::PhysicsCollider>(m_World, m_Entity);
            BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
            return;
        }
        if (!m_ActiveEdit)
        {
            m_ActiveEdit = m_Undo->BeginInteractiveEdit(name, MakeSnapshotTarget());
        }
        m_ActiveEdit.Preview([&]()
                             {
                                 applyFn();
                                 MarkOwningBodyNeedsRebuild(m_World, m_Entity);
                             });
        m_ActiveEdit.Commit();
        m_ActiveEdit = {};
        BroadcastToAdditional(applyFn, Editor::EditorChangeNotifications::ChangeKind::Commit);
    }

    void OnLayerChanging(int v)
    {
        PreviewEdit("Physics Collider Layer", [this, v]()
                    {
                        if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity))
                        {
                            int clamped = v;
                            if (clamped < 0) clamped = 0;
                            if (clamped > 31) clamped = 31;
                            c->layer = static_cast<Physics::CollisionLayer>(clamped);
                        }
                    });
    }
    void OnLayerChanged(int v) { CommitEdit("Physics Collider Layer", [this, v]() {
        if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity))
        {
            int clamped = v;
            if (clamped < 0) clamped = 0;
            if (clamped > 31) clamped = 31;
            c->layer = static_cast<Physics::CollisionLayer>(clamped);
        }
    }); }

    void OnIsTriggerChanged(bool v) { CommitEdit("Physics Collider Trigger", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->isTrigger = v; }); }
    void OnOverrideMaterialChanged(bool v) { CommitEdit("Physics Collider Override Material", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->overrideMaterial = v; }); }

    void OnFrictionChanging(float v) { PreviewEdit("Physics Collider Friction", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->material.friction = v; }); }
    void OnFrictionChanged(float v) { CommitEdit("Physics Collider Friction", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->material.friction = v; }); }

    void OnRestitutionChanging(float v) { PreviewEdit("Physics Collider Restitution", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->material.restitution = v; }); }
    void OnRestitutionChanged(float v) { CommitEdit("Physics Collider Restitution", [this, v]() { if (auto* c = m_World->GetComponentForWrite<Components::PhysicsCollider>(m_Entity)) c->material.restitution = v; }); }

    template <typename ApplyFn>
    void BroadcastToAdditional(ApplyFn&& applyFn, Editor::EditorChangeNotifications::ChangeKind kind)
    {
        ECS::EntityHandle saved = m_Entity;
        for (auto& ex : m_AdditionalEntities)
        {
            m_Entity = ex;
            applyFn();
            MarkOwningBodyNeedsRebuild(m_World, ex);
            if (m_Changes)
                m_Changes->NotifyComponentChange<Components::PhysicsCollider>(m_World, ex, kind);
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

    IntField* m_Layer = nullptr;
    Toggle* m_IsTrigger = nullptr;
    Toggle* m_OverrideMaterial = nullptr;
    FloatField* m_Friction = nullptr;
    FloatField* m_Restitution = nullptr;
};

// --- Shape inspectors ------------------------------------------------------
struct BoxShapeSnapshot
{
    float32 hx{};
    float32 hy{};
    float32 hz{};
};
struct SphereShapeSnapshot
{
    float32 r{};
};
struct CapsuleShapeSnapshot
{
    float32 r{};
    float32 hh{};
    uint8 axis{};
};
struct PlaneShapeSnapshot
{
    float32 nx{};
    float32 ny{};
    float32 nz{};
    float32 d{};
    float32 he{};
};

static void RegisterBoxShapeInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto* shape = ctx.World->GetComponent<Components::BoxColliderShape>(ctx.Entity);
        if (!shape)
            return;

        // Build UI (with undo). Capture by value so label-drag callbacks stay valid after return.
        ECS::World* world = ctx.World;
        ECS::EntityHandle entity = ctx.Entity;
        UndoRedoService* undo = ctx.Undo;
        Editor::EditorChangeNotifications* changes = ctx.ChangeNotifications;
        const auto kBoxTypeId = ECS::GetComponentTypeId<Components::BoxColliderShape>();

        auto makeTarget = [world, entity, changes, kBoxTypeId]() -> UndoRedoService::SnapshotTarget
        {
            UndoRedoService::SnapshotTarget t{};
            t.debugLabel = "BoxColliderShape";
            t.Capture = [world, entity](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
            {
                auto* s = world->GetComponent<Components::BoxColliderShape>(entity);
                if (!s)
                    return false;
                BoxShapeSnapshot snap{ s->halfExtentsX, s->halfExtentsY, s->halfExtentsZ };
                InspectorPhysicsUI::WriteBytes(out, snap);
                return true;
            };
            t.Apply = [world, entity](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
            {
                auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                if (!s)
                    return false;
                BoxShapeSnapshot snap{};
                if (!InspectorPhysicsUI::ReadBytes(in, snap))
                    return false;
                s->halfExtentsX = snap.hx;
                s->halfExtentsY = snap.hy;
                s->halfExtentsZ = snap.hz;
                MarkOwningBodyNeedsRebuild(world, entity);
                return true;
            };
            t.Notify = [changes, world, entity](Editor::EditorChangeNotifications::ChangeKind kind)
            {
                if (changes)
                    changes->NotifyComponentChange<Components::BoxColliderShape>(world, entity, kind);
            };
            return t;
        };

        auto activePtr = std::make_shared<UndoRedoService::InteractiveEdit>();
        auto preview = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo)
            {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentChange<Components::BoxColliderShape>(world, entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
                return;
            }
            if (!*activePtr)
                *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
        };
        auto commit = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo)
            {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentCommit<Components::BoxColliderShape>(world, entity);
                return;
            }
            if (!*activePtr)
                *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
            activePtr->Commit();
            *activePtr = UndoRedoService::InteractiveEdit{};
        };

        auto addFloat = [&](const char* label, float initial, std::function<void(float)> onChanging, std::function<void(float)> onChanged)
        {
            UIElement* row = InspectorPhysicsUI::AddRow(ctx.Parent);
            InspectorPhysicsUI::AddLabel(row, label);
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            auto* f = InspectorPhysicsUI::AddFloat(field, initial);
            f->SetOnValueChanging(std::move(onChanging));
            f->SetOnValueChanged(std::move(onChanged));
        };

        addFloat("Half Extents X", shape->halfExtentsX,
                 [world, entity, preview](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     preview("Box HalfExtentsX", [s, v]() { s->halfExtentsX = v; });
                 },
                 [world, entity, commit](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     commit("Box HalfExtentsX", [s, v]() { s->halfExtentsX = v; });
                 });
        addFloat("Half Extents Y", shape->halfExtentsY,
                 [world, entity, preview](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     preview("Box HalfExtentsY", [s, v]() { s->halfExtentsY = v; });
                 },
                 [world, entity, commit](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     commit("Box HalfExtentsY", [s, v]() { s->halfExtentsY = v; });
                 });
        addFloat("Half Extents Z", shape->halfExtentsZ,
                 [world, entity, preview](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     preview("Box HalfExtentsZ", [s, v]() { s->halfExtentsZ = v; });
                 },
                 [world, entity, commit](float v) {
                     auto* s = world->GetComponentForWrite<Components::BoxColliderShape>(entity);
                     if (!s) return;
                     commit("Box HalfExtentsZ", [s, v]() { s->halfExtentsZ = v; });
                 });
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::BoxColliderShape>(std::move(fn));
}

static void RegisterSphereShapeInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto* shape = ctx.World->GetComponent<Components::SphereColliderShape>(ctx.Entity);
        if (!shape)
            return;

        ECS::World* world = ctx.World;
        ECS::EntityHandle entity = ctx.Entity;
        UndoRedoService* undo = ctx.Undo;
        Editor::EditorChangeNotifications* changes = ctx.ChangeNotifications;
        const auto kTypeId = ECS::GetComponentTypeId<Components::SphereColliderShape>();

        auto makeTarget = [world, entity, changes, kTypeId]() -> UndoRedoService::SnapshotTarget
        {
            UndoRedoService::SnapshotTarget t{};
            t.debugLabel = "SphereColliderShape";
            t.Capture = [world, entity](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
            {
                auto* s = world->GetComponent<Components::SphereColliderShape>(entity);
                if (!s) return false;
                SphereShapeSnapshot snap{ s->radius };
                InspectorPhysicsUI::WriteBytes(out, snap);
                return true;
            };
            t.Apply = [world, entity](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
            {
                auto* s = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
                if (!s) return false;
                SphereShapeSnapshot snap{};
                if (!InspectorPhysicsUI::ReadBytes(in, snap)) return false;
                s->radius = snap.r;
                MarkOwningBodyNeedsRebuild(world, entity);
                return true;
            };
            t.Notify = [changes, world, entity](Editor::EditorChangeNotifications::ChangeKind kind)
            {
                if (changes)
                    changes->NotifyComponentChange<Components::SphereColliderShape>(world, entity, kind);
            };
            return t;
        };

        auto activePtr = std::make_shared<UndoRedoService::InteractiveEdit>();
        auto preview = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentChange<Components::SphereColliderShape>(world, entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
        };
        auto commit = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentCommit<Components::SphereColliderShape>(world, entity);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
            activePtr->Commit();
            *activePtr = UndoRedoService::InteractiveEdit{};
        };

        UIElement* row = InspectorPhysicsUI::AddRow(ctx.Parent);
        InspectorPhysicsUI::AddLabel(row, "Radius");
        UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
        auto* f = InspectorPhysicsUI::AddFloat(field, shape->radius);
        f->SetOnValueChanging([world, entity, preview](float v) {
            auto* s = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
            if (!s) return;
            preview("Sphere Radius", [s, v]() { s->radius = v; });
        });
        f->SetOnValueChanged([world, entity, commit](float v) {
            auto* s = world->GetComponentForWrite<Components::SphereColliderShape>(entity);
            if (!s) return;
            commit("Sphere Radius", [s, v]() { s->radius = v; });
        });
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::SphereColliderShape>(std::move(fn));
}

static void RegisterCapsuleShapeInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto* shape = ctx.World->GetComponent<Components::CapsuleColliderShape>(ctx.Entity);
        if (!shape)
            return;

        ECS::World* world = ctx.World;
        ECS::EntityHandle entity = ctx.Entity;
        UndoRedoService* undo = ctx.Undo;
        Editor::EditorChangeNotifications* changes = ctx.ChangeNotifications;
        const auto kTypeId = ECS::GetComponentTypeId<Components::CapsuleColliderShape>();

        auto makeTarget = [world, entity, changes, kTypeId]() -> UndoRedoService::SnapshotTarget
        {
            UndoRedoService::SnapshotTarget t{};
            t.debugLabel = "CapsuleColliderShape";
            t.Capture = [world, entity](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
            {
                auto* s = world->GetComponent<Components::CapsuleColliderShape>(entity);
                if (!s) return false;
                CapsuleShapeSnapshot snap{ s->radius, s->halfHeight, s->axis };
                InspectorPhysicsUI::WriteBytes(out, snap);
                return true;
            };
            t.Apply = [world, entity](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
            {
                auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
                if (!s) return false;
                CapsuleShapeSnapshot snap{};
                if (!InspectorPhysicsUI::ReadBytes(in, snap)) return false;
                s->radius = snap.r;
                s->halfHeight = snap.hh;
                s->axis = snap.axis;
                MarkOwningBodyNeedsRebuild(world, entity);
                return true;
            };
            t.Notify = [changes, world, entity](Editor::EditorChangeNotifications::ChangeKind kind)
            {
                if (changes)
                    changes->NotifyComponentChange<Components::CapsuleColliderShape>(world, entity, kind);
            };
            return t;
        };

        auto activePtr = std::make_shared<UndoRedoService::InteractiveEdit>();
        auto preview = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentChange<Components::CapsuleColliderShape>(world, entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
        };
        auto commit = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentCommit<Components::CapsuleColliderShape>(world, entity);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
            activePtr->Commit();
            *activePtr = UndoRedoService::InteractiveEdit{};
        };

        auto addFloat = [&](const char* label, float initial, std::function<void(float)> onChanging, std::function<void(float)> onChanged)
        {
            UIElement* row = InspectorPhysicsUI::AddRow(ctx.Parent);
            InspectorPhysicsUI::AddLabel(row, label);
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            auto* f = InspectorPhysicsUI::AddFloat(field, initial);
            f->SetOnValueChanging(std::move(onChanging));
            f->SetOnValueChanged(std::move(onChanged));
        };

        addFloat("Radius", shape->radius,
                 [world, entity, preview](float v) {
                     auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
                     if (!s) return;
                     preview("Capsule Radius", [s, v]() { s->radius = v; });
                 },
                 [world, entity, commit](float v) {
                     auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
                     if (!s) return;
                     commit("Capsule Radius", [s, v]() { s->radius = v; });
                 });
        addFloat("Half Height", shape->halfHeight,
                 [world, entity, preview](float v) {
                     auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
                     if (!s) return;
                     preview("Capsule HalfHeight", [s, v]() { s->halfHeight = v; });
                 },
                 [world, entity, commit](float v) {
                     auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
                     if (!s) return;
                     commit("Capsule HalfHeight", [s, v]() { s->halfHeight = v; });
                 });

        UIElement* row = InspectorPhysicsUI::AddRow(ctx.Parent);
        InspectorPhysicsUI::AddLabel(row, "Axis (0=X 1=Y 2=Z)");
        UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
        auto* axisField = InspectorPhysicsUI::AddInt(field, static_cast<int>(shape->axis));
        axisField->SetOnValueChanging([world, entity, preview](int v) {
            auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
            if (!s) return;
            int clamped = v;
            if (clamped < 0) clamped = 0;
            if (clamped > 2) clamped = 2;
            preview("Capsule Axis", [s, clamped]() { s->axis = static_cast<uint8>(clamped); });
        });
        axisField->SetOnValueChanged([world, entity, commit](int v) {
            auto* s = world->GetComponentForWrite<Components::CapsuleColliderShape>(entity);
            if (!s) return;
            int clamped = v;
            if (clamped < 0) clamped = 0;
            if (clamped > 2) clamped = 2;
            commit("Capsule Axis", [s, clamped]() { s->axis = static_cast<uint8>(clamped); });
        });
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::CapsuleColliderShape>(std::move(fn));
}

static void RegisterPlaneShapeInspector()
{
    InspectorFn fn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto* shape = ctx.World->GetComponent<Components::PlaneColliderShape>(ctx.Entity);
        if (!shape)
            return;

        // The backend plane is infinite; transform scale is ignored.
        InspectorUI::AddInfoCard(ctx.Parent,
                                 "Plane collider is infinite (transform scale is ignored). Use a "
                                 "Box collider for finite ground.");

        ECS::World* world = ctx.World;
        ECS::EntityHandle entity = ctx.Entity;
        UndoRedoService* undo = ctx.Undo;
        Editor::EditorChangeNotifications* changes = ctx.ChangeNotifications;
        const auto kTypeId = ECS::GetComponentTypeId<Components::PlaneColliderShape>();

        auto makeTarget = [world, entity, changes, kTypeId]() -> UndoRedoService::SnapshotTarget
        {
            UndoRedoService::SnapshotTarget t{};
            t.debugLabel = "PlaneColliderShape";
            t.Capture = [world, entity](UndoRedoService::SnapshotTarget::Snapshot& out) -> bool
            {
                auto* s = world->GetComponent<Components::PlaneColliderShape>(entity);
                if (!s) return false;
                PlaneShapeSnapshot snap{ s->normalX, s->normalY, s->normalZ, s->d, s->halfExtent };
                InspectorPhysicsUI::WriteBytes(out, snap);
                return true;
            };
            t.Apply = [world, entity](const UndoRedoService::SnapshotTarget::Snapshot& in) -> bool
            {
                auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity);
                if (!s) return false;
                PlaneShapeSnapshot snap{};
                if (!InspectorPhysicsUI::ReadBytes(in, snap)) return false;
                s->normalX = snap.nx;
                s->normalY = snap.ny;
                s->normalZ = snap.nz;
                s->d = snap.d;
                s->halfExtent = snap.he;
                MarkOwningBodyNeedsRebuild(world, entity);
                return true;
            };
            t.Notify = [changes, world, entity](Editor::EditorChangeNotifications::ChangeKind kind)
            {
                if (changes)
                    changes->NotifyComponentChange<Components::PlaneColliderShape>(world, entity, kind);
            };
            return t;
        };

        auto activePtr = std::make_shared<UndoRedoService::InteractiveEdit>();
        auto preview = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentChange<Components::PlaneColliderShape>(world, entity, Editor::EditorChangeNotifications::ChangeKind::Preview);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
        };
        auto commit = [activePtr, undo, makeTarget, world, entity, changes](const char* name, std::function<void()> apply)
        {
            if (!undo) {
                if (apply) apply();
                MarkOwningBodyNeedsRebuild(world, entity);
                if (changes)
                    changes->NotifyComponentCommit<Components::PlaneColliderShape>(world, entity);
                return;
            }
            if (!*activePtr) *activePtr = undo->BeginInteractiveEdit(name, makeTarget());
            activePtr->Preview([apply, world, entity]() { if (apply) apply(); MarkOwningBodyNeedsRebuild(world, entity); });
            activePtr->Commit();
            *activePtr = UndoRedoService::InteractiveEdit{};
        };

        auto addFloat = [&](const char* label, float initial, std::function<void(float)> onChanging, std::function<void(float)> onChanged)
        {
            UIElement* row = InspectorPhysicsUI::AddRow(ctx.Parent);
            InspectorPhysicsUI::AddLabel(row, label);
            UIElement* field = InspectorPhysicsUI::AddFieldContainer(row);
            auto* f = InspectorPhysicsUI::AddFloat(field, initial);
            f->SetOnValueChanging(std::move(onChanging));
            f->SetOnValueChanged(std::move(onChanged));
        };

        addFloat("Normal X", shape->normalX,
                 [world, entity, preview](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; preview("Plane NormalX", [s, v]() { s->normalX = v; }); },
                 [world, entity, commit](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; commit("Plane NormalX", [s, v]() { s->normalX = v; }); });
        addFloat("Normal Y", shape->normalY,
                 [world, entity, preview](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; preview("Plane NormalY", [s, v]() { s->normalY = v; }); },
                 [world, entity, commit](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; commit("Plane NormalY", [s, v]() { s->normalY = v; }); });
        addFloat("Normal Z", shape->normalZ,
                 [world, entity, preview](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; preview("Plane NormalZ", [s, v]() { s->normalZ = v; }); },
                 [world, entity, commit](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; commit("Plane NormalZ", [s, v]() { s->normalZ = v; }); });
        addFloat("d", shape->d,
                 [world, entity, preview](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; preview("Plane d", [s, v]() { s->d = v; }); },
                 [world, entity, commit](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; commit("Plane d", [s, v]() { s->d = v; }); });
        addFloat("Half Extent (broadphase)", shape->halfExtent,
                 [world, entity, preview](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; preview("Plane HalfExtent", [s, v]() { s->halfExtent = v; }); },
                 [world, entity, commit](float v) { auto* s = world->GetComponentForWrite<Components::PlaneColliderShape>(entity); if (!s) return; commit("Plane HalfExtent", [s, v]() { s->halfExtent = v; }); });
    };

    InspectorRegistry::Get().RegisterComponentInspector<Components::PlaneColliderShape>(std::move(fn));
}

} // namespace

void RegisterPhysicsColliderInspectors()
{
    InspectorFn colliderFn = [](const InspectorContext& ctx)
    {
        if (!ctx.Parent || !ctx.World || !ctx.Entity.IsValid())
            return;
        auto section = std::make_unique<PhysicsColliderInspectorSection>(ctx);
        ctx.Parent->AddChild(std::move(section));
    };
    InspectorRegistry::Get().RegisterComponentInspector<Components::PhysicsCollider>(std::move(colliderFn));

    RegisterBoxShapeInspector();
    RegisterSphereShapeInspector();
    RegisterCapsuleShapeInspector();
    RegisterPlaneShapeInspector();
}

} // namespace GameEngine

