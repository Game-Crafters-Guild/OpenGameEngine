#pragma once

#include "ECS/ECS.h"

#include <functional>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class EditorChangeNotifications;
class UndoRedoService;

// Runs `edit` on `entity` as one undo step named `label`, for a write that does not know in
// advance which of the entity's components it adds or changes (a component set by name, which
// can bring a required companion with it). The step holds every component of the entity whose
// bytes differ after the edit, including one the edit added or removed: undo puts back the
// bytes from before (removing what the edit added), redo the bytes from after, and each
// announces the components it wrote as an UndoRedo change. The edit's own writes are the caller's
// to announce.
//
// Returns what `edit` returned. The step covers whatever landed, so an edit refused (false) after
// it wrote is still recorded; an edit that changed nothing, or a null `undo`, records no step.
bool CommitEntityComponentsEdit(ECS::World& world, ECS::EntityHandle entity, UndoRedoService* undo,
                                EditorChangeNotifications* notifications, const std::string& label,
                                const std::function<bool()>& edit);

} // namespace GameEngine::Editor
