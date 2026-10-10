#pragma once

#include "ECS/Entity.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{

// Whether an entity may be given hand-authored children by a hierarchy drop.
//
// A generated entity (Components::RuntimeOnlyEntity) may not. It is rebuilt from
// its source and never written to the scene, so an authored child parented under
// one would serialize a `parent=` naming no entity in the file, and the loader
// hard-rejects a missing parent — a scene that fails to open, which the editor
// reports silently. SaveSceneToFile refuses that edge as well; this is the half
// that stops it being created.
//
// An invalid handle means the scene root, which always accepts children.
bool CanAcceptAuthoredChildren(ECS::World& world, ECS::EntityHandle destParent);

} // namespace GameEngine::Editor
