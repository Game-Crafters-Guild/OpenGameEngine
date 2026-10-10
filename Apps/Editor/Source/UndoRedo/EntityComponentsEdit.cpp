#include "UndoRedo/EntityComponentsEdit.h"

#include "ECS/ECSTemplates.h"
#include "ECS/Entity.h"
#include "ECS/World.h"
#include "EditorChangeNotifications.h"
#include "UndoRedo/IEditorCommand.h"
#include "UndoRedo/UndoRedoService.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

namespace
{

using ComponentBytes = std::vector<std::uint8_t>;
using EntityComponents = std::vector<std::pair<ECS::ComponentTypeId, ComponentBytes>>;

// One component the edit touched: its bytes before and after, absent on the side where the
// entity did not carry it.
struct ComponentChange
{
    ECS::ComponentTypeId TypeId = 0;
    std::optional<ComponentBytes> Before;
    std::optional<ComponentBytes> After;
};

// Every component the entity carries, by type id, in type id order.
EntityComponents CaptureEntityComponents(const ECS::World& world, ECS::EntityHandle entity)
{
    EntityComponents components;
    const ECS::Archetype* archetype = world.GetEntityArchetype(entity);
    if (!archetype)
        return components;
    for (const ECS::ComponentTypeId typeId : archetype->GetSignature().GetComponents())
    {
        ComponentBytes bytes;
        if (world.CaptureComponentBytes(entity, typeId, bytes))
            components.emplace_back(typeId, std::move(bytes));
    }
    std::sort(components.begin(), components.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return components;
}

// The components whose presence or bytes differ between two captures of one entity.
std::vector<ComponentChange> DiffEntityComponents(const EntityComponents& before, const EntityComponents& after)
{
    std::vector<ComponentChange> changes;
    auto b = before.begin();
    auto a = after.begin();
    while (b != before.end() || a != after.end())
    {
        if (a == after.end() || (b != before.end() && b->first < a->first))
        {
            changes.push_back({b->first, b->second, std::nullopt});
            ++b;
        }
        else if (b == before.end() || a->first < b->first)
        {
            changes.push_back({a->first, std::nullopt, a->second});
            ++a;
        }
        else
        {
            if (b->second != a->second)
                changes.push_back({a->first, b->second, a->second});
            ++b;
            ++a;
        }
    }
    return changes;
}

class EntityComponentsEditCommand final : public IEditorCommand
{
public:
    EntityComponentsEditCommand(std::string label, ECS::World& world, ECS::EntityHandle entity,
                                EditorChangeNotifications* notifications, std::vector<ComponentChange> changes)
        : m_Label(std::move(label)),
          m_World(world),
          m_Entity(entity),
          m_Notifications(notifications),
          m_Changes(std::move(changes))
    {
    }

    const char* GetName() const override { return m_Label.c_str(); }
    const char* GetTypeName() const override { return "EntityComponentsEditCommand"; }
    void Do() override { Apply(&ComponentChange::After); }
    void Undo() override { Apply(&ComponentChange::Before); }

private:
    void Apply(std::optional<ComponentBytes> ComponentChange::*side)
    {
        if (!m_World.IsValid(m_Entity))
            return;
        for (const ComponentChange& change : m_Changes)
        {
            const std::optional<ComponentBytes>& bytes = change.*side;
            if (bytes)
                (void)m_World.ApplyComponentBytesImmediate(m_Entity, change.TypeId, *bytes);
            else
                (void)m_World.RemoveComponentByTypeIdImmediate(m_Entity, change.TypeId);
        }
        if (!m_Notifications)
            return;
        for (const ComponentChange& change : m_Changes)
        {
            m_Notifications->NotifyComponentChanged(
                {&m_World, m_Entity, change.TypeId, EditorChangeNotifications::ChangeKind::UndoRedo});
        }
    }

    std::string m_Label;
    ECS::World& m_World;
    ECS::EntityHandle m_Entity;
    EditorChangeNotifications* m_Notifications;
    std::vector<ComponentChange> m_Changes;
};

} // namespace

bool CommitEntityComponentsEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                                EditorChangeNotifications* notifications, const std::string& label,
                                const std::function<bool()>& edit)
{
    if (!undo)
        return edit();
    const EntityComponents before = CaptureEntityComponents(world, entity);
    const bool applied = edit();
    // Diffed whatever the edit answered: a write refused part-way has already landed what it
    // wrote before refusing, and the step is what lets the user take that back.
    std::vector<ComponentChange> changes = DiffEntityComponents(before, CaptureEntityComponents(world, entity));
    if (!changes.empty())
    {
        undo->CommitAlreadyApplied(
            std::make_unique<EntityComponentsEditCommand>(label, world, entity, notifications, std::move(changes)));
    }
    return applied;
}

} // namespace GameEngine::Editor
