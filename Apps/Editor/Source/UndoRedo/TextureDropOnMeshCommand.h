#pragma once

#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECS.h"
#include "Editor/Entities/EntityMaterialTextureAssign.h"
#include "EditorChangeNotifications.h"
#include "Logger/Logger.h"
#include "UndoRedo/IEditorCommand.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor
{

// Undo step for a texture-onto-mesh drop (scene view, hierarchy panel, Polyhaven).
// Do assigns the texture to the entity's material slot and captures the state
// Undo and Redo need; run it through UndoRedoService::Execute.
//
// When the drop creates a fresh .material file (because the entity had no
// resolvable material), Undo deletes that file and unregisters it from the
// asset registry; Redo recreates the file at the same path (the project-store
// path→GUID mapping preserves its original GUID), re-registers it, and
// re-registers the runtime Material with RenderServices.
//
// A failed assignment logs an error and leaves the command inert.
class TextureDropOnMeshCommand final : public IEditorCommand
{
  public:
    using SceneDirtyFn = std::function<void()>;

    // surfaceShaderOverride names the surface shader for a material the drop
    // has to create; empty uses the default surface.
    TextureDropOnMeshCommand(std::string name,
                             ECS::World* world,
                             ECS::EntityHandle entity,
                             GUID textureGuid,
                             std::string slotKey,
                             std::string surfaceShaderOverride,
                             SceneDirtyFn onSceneDirty,
                             EditorChangeNotifications* changeNotifications)
        : m_Name(std::move(name))
        , m_World(world)
        , m_Entity(entity)
        , m_TextureGuid(textureGuid)
        , m_SlotKey(std::move(slotKey))
        , m_SurfaceShaderOverride(std::move(surfaceShaderOverride))
        , m_OnSceneDirty(std::move(onSceneDirty))
        , m_ChangeNotifications(changeNotifications)
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override
    {
        if (!m_World)
            return;

        const char* surfaceShader =
            m_SurfaceShaderOverride.empty() ? nullptr : m_SurfaceShaderOverride.c_str();
        m_Applied = AssignTextureToEntityMaterialSlot(*m_World, m_Entity, m_TextureGuid, m_SlotKey,
                                                      &m_State, surfaceShader);
        if (!m_Applied)
        {
            Logger::Log::Error("{}: could not assign texture {} to slot '{}' on entity {}",
                               m_Name, m_TextureGuid.ToString(), m_SlotKey,
                               static_cast<std::uint32_t>(m_Entity.index));
            return;
        }

        NotifyMeshRendererChanged(EditorChangeNotifications::ChangeKind::Commit);
        m_OnSceneDirty();
    }

    void Undo() override
    {
        if (!m_World || !m_Applied)
            return;

        // Point the MeshRenderer back first so the now-deleted (or about-to-be-
        // reverted) material is no longer referenced when rendering resumes.
        SetEntityMaterialPointer(*m_World, m_State.entity,
                                 m_State.prevMaterialGuid);

        if (m_State.createdNewMaterial)
        {
            RemoveCreatedMaterialFile(m_State.newMaterialGuid, m_State.newMaterialPath);
        }
        else
        {
            (void)RewriteMaterialTextureSlot(m_State.prevMaterialGuid,
                                             m_State.slotKey,
                                             m_State.prevSlotValue);
        }

        NotifyMeshRendererChanged(EditorChangeNotifications::ChangeKind::UndoRedo);
        m_OnSceneDirty();
    }

    void Redo() override
    {
        if (!m_World || !m_Applied)
            return;

        if (m_State.createdNewMaterial)
        {
            (void)RecreateMaterialFile(m_State.newMaterialPath, m_State.newMaterialJson);
        }

        SetEntityMaterialPointer(*m_World, m_State.entity,
                                 m_State.newMaterialGuid);

        const std::string newSlotValue =
            m_State.textureGuid.IsNull() ? std::string() : m_State.textureGuid.ToString();
        (void)RewriteMaterialTextureSlot(m_State.newMaterialGuid,
                                         m_State.slotKey,
                                         newSlotValue);

        NotifyMeshRendererChanged(EditorChangeNotifications::ChangeKind::UndoRedo);
        m_OnSceneDirty();
    }

  private:
    void NotifyMeshRendererChanged(EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_ChangeNotifications || !m_World || !m_State.entity.IsValid())
            return;
        EditorChangeNotifications::ComponentChangedEvent e{};
        e.world = m_World;
        e.entity = m_State.entity;
        e.componentType = ECS::GetComponentTypeId<Components::MeshRenderer>();
        e.kind = kind;
        m_ChangeNotifications->NotifyComponentChanged(e);
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;
    ECS::EntityHandle m_Entity{};
    GUID m_TextureGuid;
    std::string m_SlotKey;
    std::string m_SurfaceShaderOverride;
    TextureOnMeshAssignState m_State;
    bool m_Applied = false;
    SceneDirtyFn m_OnSceneDirty;
    EditorChangeNotifications* m_ChangeNotifications = nullptr; // not owned
};

} // namespace GameEngine::Editor
