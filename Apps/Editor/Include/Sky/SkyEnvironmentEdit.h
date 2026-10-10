#pragma once

#include "Components/Rendering/SkyEnvironment.h" // SkyMode
#include "ECS/Entity.h"

#include <functional>
#include <string>
#include <vector>

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Apply one edit to the Sky Environment on `primary` and on every entity in `extras` under a
// single undo step. The inspector's row helpers cover value-typed rows; a discrete action (a
// toggle, a switch, a relink) needs the same multi-entity lifecycle, and one undo entry per entity
// would make a multi-select edit take that many undos to take back. When the edit starts the sky's
// drive of its sun light, the light's colour is recorded in the same step, so undoing the edit puts
// it back (the sky system hands back a light whose drive ends by itself).
void CommitSkyEnvironmentEdit(ECS::World* world, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                              EditorChangeNotifications* notifications, UndoRedoService* undo,
                              const std::string& label, const std::function<void(Components::SkyEnvironment&)>& edit);

// Switch the Sky Mode (Physical or Gradient) of the sky on `primary` and of every sky in `extras`
// as one undo step, and rebuild their inspectors: the two modes show different rows.
void SwitchSkyMode(ECS::World* world, ECS::EntityHandle primary, const std::vector<ECS::EntityHandle>& extras,
                   EditorChangeNotifications* notifications, UndoRedoService* undo, Components::SkyMode mode);

} // namespace GameEngine::Editor
