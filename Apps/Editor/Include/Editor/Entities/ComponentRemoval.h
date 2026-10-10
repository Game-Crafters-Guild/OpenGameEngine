#pragma once

#include "ECS/ECS.h"

#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Removes one component from `entity`. With `undo` set it is one undo step named `label`, holding
// the removal and what it makes a system overwrite on other entities (CommitGenericEdit): removing
// an HDRI Skybox hands the sun light to a Sky Environment. Removing a Post Process Volume also
// removes every post-process effect on the entity, and undo restores them all.
void CommitComponentRemoval(ECS::World& world, UndoRedoService* undo, EditorChangeNotifications* notifications,
                            ECS::EntityHandle entity, ECS::ComponentTypeId typeId, const std::string& label);

} // namespace GameEngine::Editor
