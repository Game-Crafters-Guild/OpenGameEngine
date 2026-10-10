#include "Editor/Entities/ComponentRemoval.h"

#include "Components/Rendering/PostProcessVolume.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "Engine/Rendering/PostProcessEffectRegistry.h"
#include "UndoRedo/GenericEditUndo.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
void NotifyComponentChanged(EditorChangeNotifications* notifications, ECS::World* world, ECS::EntityHandle entity,
                            ECS::ComponentTypeId typeId)
{
    if (!notifications)
        return;
    notifications->NotifyComponentChanged({world, entity, typeId, EditorChangeNotifications::ChangeKind::Commit});
}

class RemoveComponentCommand final : public IEditorCommand
{
  public:
    RemoveComponentCommand(std::string name,
                           ECS::World* world,
                           EditorChangeNotifications* notifications,
                           ECS::EntityHandle entity,
                           ECS::ComponentTypeId typeId)
        : m_Name(std::move(name)), m_World(world), m_Notifications(notifications), m_Entity(entity), m_TypeId(typeId)
    {
        if (m_World && m_Entity.IsValid() && m_World->IsValid(m_Entity))
        {
            const ECS::ComponentTypeId postProcessVolumeId =
                ECS::GetComponentTypeId<Components::PostProcessVolume>();
            m_RemovePostFxBundle = (m_TypeId == postProcessVolumeId);

            if (m_RemovePostFxBundle)
            {
                // Volume snapshot FIRST — Redo() removes effects before the
                // Volume via m_Snapshots.front(), Undo() restores in capture
                // order. The effect set is the registry: every registered
                // descriptor is a Volume-owned effect (doc §4 cascade policy).
                CaptureSnapshot(postProcessVolumeId);
                Rendering::PostProcessEffectRegistry::ForEach(
                    [this](const Rendering::PostProcessEffectDescriptor& d)
                    { CaptureSnapshot(d.Type); });

                for (const auto& snap : m_Snapshots)
                {
                    if (snap.HadComponent)
                    {
                        m_HadComponent = true;
                        break;
                    }
                }
            }
            else
            {
                m_HadComponent = m_World->CaptureComponentBytes(m_Entity, m_TypeId, m_SnapshotBytes);
            }
        }
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (!m_HadComponent)
            return;

        if (m_RemovePostFxBundle)
        {
            for (const auto& snap : m_Snapshots)
            {
                if (!snap.HadComponent)
                    continue;
                if (m_World->ApplyComponentBytesImmediate(m_Entity, snap.TypeId, snap.Bytes))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, snap.TypeId);
            }
            return;
        }

        if (m_World->ApplyComponentBytesImmediate(m_Entity, m_TypeId, m_SnapshotBytes))
        {
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
    }

    void Redo() override
    {
        if (!m_World || !m_Entity.IsValid() || !m_World->IsValid(m_Entity))
            return;
        if (!m_HadComponent)
            return;

        if (m_RemovePostFxBundle)
        {
            for (size_t i = 1; i < m_Snapshots.size(); ++i)
            {
                const auto& snap = m_Snapshots[i];
                if (!snap.HadComponent)
                    continue;
                if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, snap.TypeId))
                    NotifyComponentChanged(m_Notifications, m_World, m_Entity, snap.TypeId);
            }
            const auto& volumeSnap = m_Snapshots.front();
            if (volumeSnap.HadComponent &&
                m_World->RemoveComponentByTypeIdImmediate(m_Entity, volumeSnap.TypeId))
            {
                NotifyComponentChanged(m_Notifications, m_World, m_Entity, volumeSnap.TypeId);
            }
            return;
        }

        if (m_World->RemoveComponentByTypeIdImmediate(m_Entity, m_TypeId))
        {
            NotifyComponentChanged(m_Notifications, m_World, m_Entity, m_TypeId);
        }
    }

  private:
    struct ComponentSnapshot
    {
        ECS::ComponentTypeId TypeId{};
        bool HadComponent = false;
        std::vector<uint8_t> Bytes;
    };

    void CaptureSnapshot(ECS::ComponentTypeId typeId)
    {
        ComponentSnapshot snapshot{};
        snapshot.TypeId = typeId;
        snapshot.HadComponent = m_World->CaptureComponentBytes(m_Entity, typeId, snapshot.Bytes);
        m_Snapshots.push_back(std::move(snapshot));
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;                                // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    ECS::EntityHandle m_Entity{};
    ECS::ComponentTypeId m_TypeId{};
    bool m_HadComponent = false;
    bool m_RemovePostFxBundle = false;
    std::vector<uint8_t> m_SnapshotBytes;
    std::vector<ComponentSnapshot> m_Snapshots;
};
} // namespace

void CommitComponentRemoval(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                            ECS::EntityHandle entity, ECS::ComponentTypeId typeId, const std::string& label)
{
    if (!undo)
    {
        RemoveComponentCommand(label, &world, notifications, entity, typeId).Redo();
        return;
    }
    CommitGenericEdit(world, *undo, label, [&]() {
        undo->Execute(std::make_unique<RemoveComponentCommand>(label, &world, notifications, entity, typeId));
    });
}

} // namespace GameEngine::Editor
