#pragma once

#include "ECS/ECS.h"

#include <span>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// The entity toggle: the hierarchy row's icon, the inspector header's switch and the debug server's
// set_entity_enabled. Every entity of `entities` takes `enabled` as its own state (Entity::SetEnabled),
// in one undo step when `undo` is set, announced as a world-structure change; a click that switches
// nothing records nothing. Descendants keep their own state and follow through the hierarchy pass
// (ECS::DisabledInHierarchySystem), so a child that was off on its own stays off when its parent comes
// back on.
void CommitEntityEnabledToggle(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                               std::span<const ECS::EntityHandle> entities, bool enabled);

// An entity's on/off state as the editor shows it.
struct EntityActivity
{
    bool SwitchedOn = true;        // its own state (Entity::IsEnabled)
    bool ActiveInHierarchy = true; // on together with every ancestor (Entity::IsEnabledInHierarchy)
    // While the entity is on but inactive: the nearest ancestor that is switched off itself.
    ECS::EntityHandle OffAncestor{};

    bool operator==(const EntityActivity&) const = default;
};

EntityActivity DescribeEntityActivity(ECS::World& world, ECS::EntityHandle entity);

// The state in words, for the hierarchy row and the inspector header: "Off" for an entity switched off
// itself, "Inactive: <ancestor> is off" for one that is on under a switched-off ancestor, and empty for
// an active entity.
std::string EntityActivityReason(ECS::World& world, const EntityActivity& activity);

} // namespace GameEngine::Editor
