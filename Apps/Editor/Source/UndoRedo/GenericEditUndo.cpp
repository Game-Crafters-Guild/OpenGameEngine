#include "UndoRedo/GenericEditUndo.h"

#include "Editor/Entities/EditorComponentTraits.h"
#include "UndoRedo/UndoRedoService.h"

#include <memory>
#include <utility>
#include <vector>

namespace GameEngine::Editor
{

void CommitGenericEdit(ECS::World& world, UndoRedoService& undo, const std::string& label,
                       const std::function<void()>& applyEdit)
{
    std::vector<GenericEditUndoRecorder> recorders;
    for (const auto& [typeId, traits] : EditorComponentTraitsRegistry::Get().Snapshot())
    {
        (void)typeId;
        if (!traits.BeforeGenericEdit)
            continue;
        if (GenericEditUndoRecorder recorder = traits.BeforeGenericEdit(world))
            recorders.push_back(std::move(recorder));
    }

    // Scope exit ends the compound even if the edit or a recorder throws: an unbalanced compound
    // stack would fold every later edit into this step.
    undo.BeginCompound(label);
    const std::shared_ptr<void> compoundScope(static_cast<void*>(nullptr),
                                              [&undo](void*) { undo.EndCompound(); });
    applyEdit();
    for (const GenericEditUndoRecorder& recorder : recorders)
        recorder(undo, label);
}

} // namespace GameEngine::Editor
