#pragma once

#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/Entity.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameEngine
{
namespace ECS
{
class World;
}

namespace Editor
{
class EditorChangeNotifications;

// One undo step for an entity duplication. The duplication itself
// (DuplicateEntitySubtreeRoots) runs at the call site before this command is
// constructed, so it is committed via CommitAlreadyApplied; construction
// captures the clones' component bytes.
//
// - Undo: preserve-handle destroy of every cloned entity, then onClonesRemoved
//   (restore the pre-duplicate selection).
// - Redo: revive the SAME handles and re-apply the captured bytes, then
//   onClonesRestored (re-select the clones). Redo must never re-clone — fresh
//   handles would break later history entries that reference the clones.
//
// Both directions are the exact inverse of a delete of the clones, so this
// delegates to a DeleteEntitiesCommand with Undo/Redo swapped. The selection
// callbacks are panel-specific and must suppress their own selection-undo so
// the whole duplication stays a single undo entry.
class DuplicateEntitiesCommand final : public IEditorCommand
{
  public:
    DuplicateEntitiesCommand(std::string name,
                             ECS::World* world,
                             EditorChangeNotifications* notifications,
                             std::vector<ECS::EntityHandle> clonedEntities,
                             std::function<void()> onClonesRestored,
                             std::function<void()> onClonesRemoved)
        : m_Name(std::move(name)),
          m_OnClonesRestored(std::move(onClonesRestored)),
          m_OnClonesRemoved(std::move(onClonesRemoved))
    {
        m_Inverse = std::make_unique<DeleteEntitiesCommand>(m_Name, world, notifications, std::move(clonedEntities));
    }

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "DuplicateEntitiesCommand"; }

    // Not used by the CommitAlreadyApplied path; routed to Redo so an
    // Execute-style caller can never re-clone.
    void Do() override { Redo(); }

    void Undo() override
    {
        if (m_Inverse)
            m_Inverse->Redo(); // preserve-handle destroy of the clones
        if (m_OnClonesRemoved)
            m_OnClonesRemoved(); // restore pre-duplicate selection
    }

    void Redo() override
    {
        if (m_Inverse)
            m_Inverse->Undo(); // revive the same handles + re-apply captured bytes
        if (m_OnClonesRestored)
            m_OnClonesRestored(); // clones valid again before re-selecting them
    }

  private:
    std::string m_Name;
    std::unique_ptr<DeleteEntitiesCommand> m_Inverse;
    std::function<void()> m_OnClonesRestored;
    std::function<void()> m_OnClonesRemoved;
};

} // namespace Editor
} // namespace GameEngine
