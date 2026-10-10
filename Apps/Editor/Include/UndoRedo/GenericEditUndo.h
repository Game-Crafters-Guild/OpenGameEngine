#pragma once

#include <functional>
#include <string>

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor
{
class UndoRedoService;

// Runs `applyEdit` as one undo step named `label`, for editor code that edits a component or an
// entity without knowing what it carries (a component's enable toggle, an entity's creation). The
// step holds the commands `applyEdit` commits, then what each component's
// EditorComponentTraits::BeforeGenericEdit records about the edit's effect on other entities.
void CommitGenericEdit(ECS::World& world, UndoRedoService& undo, const std::string& label,
                       const std::function<void()>& applyEdit);

} // namespace GameEngine::Editor
