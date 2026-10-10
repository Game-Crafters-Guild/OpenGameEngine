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

// One undo step for a delete that also restores selection. Do/Redo deletes the entities and
// runs onDeleted (deselect); Undo revives them (valid again) then runs onRevived (re-select),
// so a single Ctrl+Z brings the entities back AND re-selects them, and redo re-deletes and
// deselects. The selection callbacks are panel-specific (Hierarchy tree vs Scene View) and
// must suppress their own selection-undo so the whole thing stays a single undo entry.
class DeleteEntitiesSelectionCommand final : public IEditorCommand
{
  public:
    DeleteEntitiesSelectionCommand(std::string name,
                                   ECS::World* world,
                                   EditorChangeNotifications* notifications,
                                   std::vector<ECS::EntityHandle> entities,
                                   std::function<void()> onDeleted,
                                   std::function<void()> onRevived)
        : m_Name(std::move(name)), m_OnDeleted(std::move(onDeleted)), m_OnRevived(std::move(onRevived))
    {
        m_Delete = std::make_unique<DeleteEntitiesCommand>(m_Name, world, notifications, std::move(entities));
    }

    const char* GetName() const override { return m_Name.c_str(); }
    const char* GetTypeName() const override { return "DeleteEntitiesCommand"; }

    void Do() override { Redo(); }

    void Redo() override
    {
        if (m_Delete)
            m_Delete->Redo(); // delete
        if (m_OnDeleted)
            m_OnDeleted(); // deselect
    }

    void Undo() override
    {
        if (m_Delete)
            m_Delete->Undo(); // revive (entities valid again before we re-select them)
        if (m_OnRevived)
            m_OnRevived(); // re-select
    }

  private:
    std::string m_Name;
    std::unique_ptr<DeleteEntitiesCommand> m_Delete;
    std::function<void()> m_OnDeleted;
    std::function<void()> m_OnRevived;
};

} // namespace Editor
} // namespace GameEngine
