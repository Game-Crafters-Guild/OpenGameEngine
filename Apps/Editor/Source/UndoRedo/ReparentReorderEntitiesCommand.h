#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"

#include "Components/Hierarchy.h"
#include "Components/Transform.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"

namespace GameEngine::Editor
{
// Some Windows SDK headers define an `Undo` macro which breaks qualified enum values like
// `EditorChangeNotifications::ChangeKind::Undo`.
#if defined(Undo)
#undef Undo
#endif

// Reparent and reorder entities as a single undoable command.
// Uses Components::Parent and Components::HierarchyOrder to persist the hierarchy + sibling order.
class ReparentReorderEntitiesCommand final : public IEditorCommand
{
  public:
    struct EntityState
    {
        ECS::EntityHandle entity{};
        bool hadParent = false;
        Components::Parent parent{};
        bool hadOrder = false;
        Components::HierarchyOrder order{};
        bool hadTransform = false;
        Components::Transform transform{};
    };

    ReparentReorderEntitiesCommand(std::string name,
                                  ECS::World* world,
                                  EditorChangeNotifications* notifications,
                                  std::vector<EntityState> before,
                                  std::vector<EntityState> after)
        : m_Name(std::move(name))
        , m_World(world)
        , m_Notifications(notifications)
        , m_Before(std::move(before))
        , m_After(std::move(after))
    {
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    // Some Windows headers define `Undo` macros that also interfere with qualified enum values.
    // The change notification kind is best-effort; use Commit for both directions for now.
    void Undo() override { ApplyStates(m_Before, /*kind=*/EditorChangeNotifications::ChangeKind::Commit); }
    void Redo() override { ApplyStates(m_After, /*kind=*/EditorChangeNotifications::ChangeKind::Commit); }

  private:
    void ApplyStates(const std::vector<EntityState>& states, EditorChangeNotifications::ChangeKind kind)
    {
        if (!m_World)
            return;

        for (const auto& s : states)
        {
            if (!s.entity.IsValid() || !m_World->IsValid(s.entity))
                continue;

            if (s.hadParent && s.parent.parent.IsValid())
            {
                m_World->AddComponentImmediate(s.entity, s.parent);
            }
            else
            {
                m_World->RemoveComponentImmediate<Components::Parent>(s.entity);
            }

            if (s.hadOrder)
            {
                m_World->AddComponentImmediate(s.entity, s.order);
            }
            else
            {
                m_World->RemoveComponentImmediate<Components::HierarchyOrder>(s.entity);
            }

            if (s.hadTransform)
                m_World->AddComponentImmediate(s.entity, s.transform);
        }

        if (m_Notifications)
        {
            EditorChangeNotifications::WorldStructureChangedEvent e{};
            e.world = m_World;
            e.kind = kind;
            m_Notifications->NotifyWorldStructureChanged(e);
        }
    }

  private:
    std::string m_Name;
    ECS::World* m_World = nullptr;                       // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    std::vector<EntityState> m_Before;
    std::vector<EntityState> m_After;
};

} // namespace GameEngine::Editor

