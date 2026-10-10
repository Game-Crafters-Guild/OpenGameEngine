#pragma once

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include <functional>
#include <string>
#include <utility>

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// Undo step for a material-asset-onto-mesh drop (scene view, hierarchy panel).
// Do captures the MeshRenderer's current material and assigns the dropped one;
// run it through UndoRedoService::Execute. Undo and Redo swap the GUID.
class MaterialDropOnMeshCommand final : public IEditorCommand
{
  public:
    using SceneDirtyFn = std::function<void()>;

    MaterialDropOnMeshCommand(std::string name,
                              ECS::World* world,
                              ECS::EntityHandle entity,
                              GUID newMaterialGuid,
                              SceneDirtyFn onSceneDirty,
                              EditorChangeNotifications* changeNotifications)
        : m_Name(std::move(name))
        , m_World(world)
        , m_Entity(entity)
        , m_NewMaterialGuid(newMaterialGuid)
        , m_OnSceneDirty(std::move(onSceneDirty))
        , m_ChangeNotifications(changeNotifications)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override
    {
        if (!m_World)
            return;
        if (const auto* mr = m_World->GetComponent<Components::MeshRenderer>(m_Entity))
            m_PrevMaterialGuid = mr->materialAssetGuid.ToGuid();
        SetEntityMaterialPointer(*m_World, m_Entity, m_NewMaterialGuid);
        NotifyChanged(EditorChangeNotifications::ChangeKind::Commit);
        m_OnSceneDirty();
    }

    void Undo() override
    {
        if (!m_World)
            return;
        SetEntityMaterialPointer(*m_World, m_Entity, m_PrevMaterialGuid);
        NotifyChanged(EditorChangeNotifications::ChangeKind::UndoRedo);
        m_OnSceneDirty();
    }

    void Redo() override
    {
        if (!m_World)
            return;
        SetEntityMaterialPointer(*m_World, m_Entity, m_NewMaterialGuid);
        NotifyChanged(EditorChangeNotifications::ChangeKind::UndoRedo);
        m_OnSceneDirty();
    }

  private:
    void NotifyChanged(EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_ChangeNotifications || !m_World || !m_Entity.IsValid())
            return;
        EditorChangeNotifications::ComponentChangedEvent e{};
        e.world = m_World;
        e.entity = m_Entity;
        e.componentType = ECS::GetComponentTypeId<Components::MeshRenderer>();
        e.kind = kind;
        m_ChangeNotifications->NotifyComponentChanged(e);
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    GUID m_PrevMaterialGuid;
    GUID m_NewMaterialGuid;
    SceneDirtyFn m_OnSceneDirty;
    EditorChangeNotifications* m_ChangeNotifications = nullptr; // not owned
};

} // namespace GameEngine::Editor
