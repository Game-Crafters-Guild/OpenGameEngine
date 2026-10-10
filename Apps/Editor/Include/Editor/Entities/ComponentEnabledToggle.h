#pragma once

#include "ECS/ECS.h"

#include <span>
#include <string>
#include <string_view>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// The component section header's enable dot. It shows and switches a component's on/off state:
// the component's ECS::ComponentDisabled tag, or, for a type that keeps its own Enabled field
// (ECS::ComponentFlags::KeepsOwnEnabledField: post-process effects, terrain effects, ValueCurve),
// that field. A NotToggleable type (Transform, Name) has no dot.

// Reads the state the dot shows. False when the entity does not carry the component and for a type
// that has no dot.
bool TryGetComponentEnabled(ECS::World* world, ECS::EntityHandle entity, ECS::ComponentTypeId typeId,
                            bool& outEnabled);

// Whether the entities of a multi-selection that carry the component disagree on the state
// TryGetComponentEnabled reads. `selection` is every selected entity, as InspectorContext::Entities
// holds it (empty for a single selection, which never disagrees). The dot then shows the mixed
// state, and a click switches every one of them to one state.
bool IsComponentEnabledMixed(ECS::World* world, std::span<const ECS::EntityHandle> selection,
                             ECS::ComponentTypeId typeId);

// The dot's tooltip: the one its owner registered for its type (EditorComponentTraits::
// EnableToggleTooltip) or, when it has none, the one the component hosting its section registered
// for every entry it hosts (EditorComponentTraits::HostedEnableToggleTooltip), or else the generic
// one; then, when the entities of `selection` disagree (IsComponentEnabledMixed), which way a click
// switches them all: the opposite of `primary`'s state.
std::string ComponentEnabledToggleTooltip(ECS::World* world, ECS::EntityHandle primary,
                                          std::span<const ECS::EntityHandle> selection, ECS::ComponentTypeId typeId);

// A click on the dot. Switches the component to `enabled` on `primary` and on every other entity of
// `selection` (as for IsComponentEnabledMixed) that carries it, as one SetComponentEnabledCommand
// holding each entity's state before the click. With `undo` set that command is one undo step,
// together with what the switch makes a system overwrite on other entities (CommitGenericEdit),
// named after `componentTitle`, the name the section header shows: "Switch Audio Emitter on
// (3 entities)". Without it the switch is applied and each change notified. A click that switches
// nothing (no entity has a switch for the type, or all have the clicked state) records nothing.
void CommitComponentEnabledToggle(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                                  ECS::EntityHandle primary, std::span<const ECS::EntityHandle> selection,
                                  ECS::ComponentTypeId typeId, std::string_view componentTitle, bool enabled);

} // namespace GameEngine::Editor
