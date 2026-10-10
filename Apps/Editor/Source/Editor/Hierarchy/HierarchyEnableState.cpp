#include "Editor/Hierarchy/HierarchyEnableState.h"

#include "Components/Hierarchy.h"
#include "ECS/Components.h"
#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "Editor/Entities/EntityDisplayName.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{
namespace
{
// One click of the entity toggle over a selection. Each entity's own state before the click is the
// snapshot undo puts back; descendants are never written, so they need none.
class SetEntityEnabledCommand final : public IEditorCommand
{
  public:
    SetEntityEnabledCommand(ECS::World* world, EditorChangeNotifications* notifications,
                            std::span<const ECS::EntityHandle> entities, bool enabled)
        : m_World(world), m_Notifications(notifications), m_Enabled(enabled)
    {
        m_Entries.reserve(entities.size());
        for (ECS::EntityHandle entity : entities)
        {
            if (entity.IsValid() && m_World->IsValid(entity))
                m_Entries.push_back({entity, ECS::Entity(m_World, entity).IsEnabled()});
        }
        m_Name = Label();
    }

    const char* GetName() const override { return m_Name.c_str(); }
    void Do() override { Redo(); }

    void Undo() override
    {
        for (const Entry& entry : m_Entries)
            Apply(entry.Entity, entry.WasEnabled);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    void Redo() override
    {
        for (const Entry& entry : m_Entries)
            Apply(entry.Entity, m_Enabled);
        NotifyWorldStructure(m_Notifications, m_World);
    }

    // False when the selection holds no entity, or every one already has the clicked state.
    bool ChangesAnything() const
    {
        for (const Entry& entry : m_Entries)
        {
            if (entry.WasEnabled != m_Enabled)
                return true;
        }
        return false;
    }

  private:
    struct Entry
    {
        ECS::EntityHandle Entity{};
        bool WasEnabled = true;
    };

    // The undo step's name: the entity it switched, or how many, and which way.
    std::string Label() const
    {
        const char* direction = m_Enabled ? " on" : " off";
        if (m_Entries.size() == 1)
            return "Switch " + EntityDisplayName(*m_World, m_Entries.front().Entity) + direction;
        return "Switch " + std::to_string(m_Entries.size()) + " entities" + direction;
    }

    void Apply(ECS::EntityHandle entity, bool enabled)
    {
        if (m_World && m_World->IsValid(entity))
            ECS::Entity(m_World, entity).SetEnabled(enabled);
    }

    std::string m_Name;
    ECS::World* m_World = nullptr;                        // not owned
    EditorChangeNotifications* m_Notifications = nullptr; // not owned
    bool m_Enabled = true;
    std::vector<Entry> m_Entries;
};
} // namespace

void CommitEntityEnabledToggle(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                               std::span<const ECS::EntityHandle> entities, bool enabled)
{
    auto command = std::make_unique<SetEntityEnabledCommand>(&world, notifications, entities, enabled);
    if (!command->ChangesAnything())
        return;
    if (!undo)
    {
        command->Redo();
        return;
    }
    undo->Execute(std::move(command));
}

EntityActivity DescribeEntityActivity(ECS::World& world, ECS::EntityHandle entity)
{
    const ECS::Entity handle(&world, entity);
    EntityActivity activity;
    activity.SwitchedOn = handle.IsEnabled();
    activity.ActiveInHierarchy = handle.IsEnabledInHierarchy();
    if (!activity.SwitchedOn || activity.ActiveInHierarchy)
        return activity;
    for (const auto* link = world.GetComponent<Components::Parent>(entity); link && world.IsValid(link->parent);
         link = world.GetComponent<Components::Parent>(link->parent))
    {
        if (ECS::Entity(&world, link->parent).IsEnabled())
            continue;
        activity.OffAncestor = link->parent;
        break;
    }
    return activity;
}

std::string EntityActivityReason(ECS::World& world, const EntityActivity& activity)
{
    if (!activity.SwitchedOn)
        return "Off";
    if (activity.ActiveInHierarchy)
        return {};
    if (!activity.OffAncestor.IsValid() || !world.IsValid(activity.OffAncestor))
        return "Inactive: a parent is off";
    return "Inactive: " + EntityDisplayName(world, activity.OffAncestor) + " is off";
}

} // namespace GameEngine::Editor
