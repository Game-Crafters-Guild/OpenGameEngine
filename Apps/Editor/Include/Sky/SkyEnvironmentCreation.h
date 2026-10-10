#pragma once

#include "ECS/ECS.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Creates a root entity carrying a Sky Environment linked to the first directional light in the
// world, as one undo step when `undo` is set. When the new sky is the one the sky system renders
// it drives that light from its next frame, so the step also records the light's color and undoing
// the creation hands it back (CommitGenericEdit). Returns the new entity, invalid if none was made.
ECS::EntityHandle CreateSkyEnvironmentEntity(ECS::World& world, UndoRedoService* undo,
                                             EditorChangeNotifications* notifications);

} // namespace GameEngine::Editor
