#pragma once

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "AssetCore/GUID.h"
#include "Components/Name.h"
#include "Components/Terrain/TerrainModifiers.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "TerrainECS/TerrainService.h"
#include "TerrainECS/TerrainZonePayload.h"

#include <utility>

namespace GameEngine::Editor
{

// Full state of the transformable zone entity a brush stroke touches. Auto-grow
// changes the component extent + transform mid-stroke, so a stroke undo must
// revert those WITH the payload (edit-pipeline design §8).
struct ZoneStrokeState
{
    bool IsPaint = false;
    Components::TerrainSculptZone Sculpt{};
    Components::TerrainPaintZone Paint{};
    Components::Transform Transform{};
    Components::WorldTransform WorldTransform{};
    Components::Name Name{};
    TerrainECS::TerrainZonePayload Payload;
};

// One undo entry for a whole terrain brush stroke — surgical (no whole-world
// snapshot). Follows the DeleteEntitiesCommand invariants so the undo stack
// stays consistent:
//   * the zone entity handle is PRESERVED across undo/redo
//     (DestroyEntityImmediatePreserveHandle / ReviveEntityImmediatePreserveHandle),
//     so later commands that captured it don't silently no-op after a free-list
//     handle reuse;
//   * every Undo()/Redo() fires a WorldStructureChanged event, so the
//     event-driven hierarchy panel doesn't keep a stale row for a
//     destroyed/created entity.
//
// A stroke that auto-created its zone destroys+revives the entity; a stroke on
// an existing zone restores its component + transform + payload (reverting any
// mid-stroke auto-grow).
class TerrainZoneStrokeCommand final : public IEditorCommand
{
public:
    TerrainZoneStrokeCommand(ECS::World* world, EditorChangeNotifications* notifications,
                             TerrainECS::TerrainService* service, ECS::EntityHandle entity,
                             GUID guid, bool created, ZoneStrokeState before, ZoneStrokeState after)
        : m_World(world), m_Notifications(notifications), m_Service(service),
          m_Entity(entity), m_Guid(guid), m_Created(created),
          m_Before(std::move(before)), m_After(std::move(after)) {}

    const char* GetName() const override { return "Terrain Brush Stroke"; }
    const char* GetTypeName() const override { return "TerrainZoneStrokeCommand"; }

    void Do() override { Redo(); }

    void Redo() override
    {
        if (m_Created)
            ReviveZone(m_After);
        else
            RestoreZone(m_After);
        Notify();
    }

    void Undo() override
    {
        if (m_Created)
            DestroyZone();
        else
            RestoreZone(m_Before);
        Notify();
    }

private:
    void DestroyZone()
    {
        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
            m_World->DestroyEntityImmediatePreserveHandle(m_Entity);
        if (m_Service)
            m_Service->EvictZonePayload(m_Guid);
    }

    void ReviveZone(const ZoneStrokeState& s)
    {
        if (!m_World || !m_Entity.IsValid())
            return;
        if (!m_World->IsValid(m_Entity))
            (void)m_World->ReviveEntityImmediatePreserveHandle(m_Entity);
        RestoreZone(s);
    }

    void RestoreZone(const ZoneStrokeState& s)
    {
        if (m_World && m_World->IsValid(m_Entity))
        {
            m_World->AddComponentImmediate(m_Entity, s.Transform);
            m_World->AddComponentImmediate(m_Entity, s.WorldTransform);
            m_World->AddComponentImmediate(m_Entity, s.Name);
            if (s.IsPaint)
                m_World->AddComponentImmediate(m_Entity, s.Paint);
            else
                m_World->AddComponentImmediate(m_Entity, s.Sculpt);
        }
        if (m_Service)
            m_Service->SetZonePayload(m_Guid, s.Payload);
    }

    void Notify()
    {
        if (!m_Notifications || !m_World)
            return;
        EditorChangeNotifications::WorldStructureChangedEvent e{};
        e.world = m_World;
        e.kind = EditorChangeNotifications::ChangeKind::Commit;
        m_Notifications->NotifyWorldStructureChanged(e);
    }

    ECS::World* m_World = nullptr;                         // not owned
    EditorChangeNotifications* m_Notifications = nullptr;  // not owned
    TerrainECS::TerrainService* m_Service = nullptr;       // not owned
    ECS::EntityHandle m_Entity;
    GUID m_Guid;
    bool m_Created;
    ZoneStrokeState m_Before;
    ZoneStrokeState m_After;
};

} // namespace GameEngine::Editor
