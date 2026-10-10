#pragma once

#include "UndoRedo/DeleteEntitiesCommand.h"
#include "UndoRedo/IEditorCommand.h"

#include "ECS/Entity.h"
#include "UI/Interaction/Types.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <unordered_set>
#include <vector>

namespace GameEngine::Editor
{

// Single undo step for a Scene View asset drop: removes spawned entities on Undo (same capture
// semantics as DeleteEntitiesCommand) and restores hierarchy selection before/after the drop.
class SceneViewDropUndoCommand final : public IEditorCommand
{
  public:
    using ApplySelectionFn =
        std::function<void(const std::vector<UI::Interaction::ItemId>&, UI::Interaction::ItemId)>;

    SceneViewDropUndoCommand(std::string name,
                             ECS::World* world,
                             EditorChangeNotifications* notifications,
                             std::vector<ECS::EntityHandle> droppedRoots,
                             std::vector<UI::Interaction::ItemId> selectionBefore,
                             UI::Interaction::ItemId anchorBefore,
                             std::vector<UI::Interaction::ItemId> selectionAfter,
                             UI::Interaction::ItemId anchorAfter,
                             ApplySelectionFn applySelection)
        : m_Name(std::move(name))
        , m_SelectionBefore(std::move(selectionBefore))
        , m_AnchorBefore(anchorBefore)
        , m_SelectionAfter(std::move(selectionAfter))
        , m_AnchorAfter(anchorAfter)
        , m_ApplySelection(std::move(applySelection))
    {
        std::vector<ECS::EntityHandle> merged = MergeDroppedSubtrees(*world, droppedRoots);
        m_Delete = std::make_unique<DeleteEntitiesCommand>(m_Name, world, notifications, std::move(merged));
    }

    const char* GetName() const override { return m_Name.c_str(); }

    void Do() override {}

    void Undo() override
    {
        if (m_Delete)
        {
            m_Delete->Redo();
        }
        if (m_ApplySelection)
        {
            m_ApplySelection(m_SelectionBefore, m_AnchorBefore);
        }
    }

    void Redo() override
    {
        if (m_Delete)
        {
            m_Delete->Undo();
        }
        if (m_ApplySelection)
        {
            m_ApplySelection(m_SelectionAfter, m_AnchorAfter);
        }
    }

  private:
    static std::vector<ECS::EntityHandle> MergeDroppedSubtrees(ECS::World& world,
                                                               const std::vector<ECS::EntityHandle>& roots)
    {
        std::unordered_set<std::uint32_t> seen;
        std::vector<ECS::EntityHandle> out;
        seen.reserve(static_cast<size_t>(roots.size()) * 16u);
        out.reserve(static_cast<size_t>(roots.size()) * 16u);

        for (ECS::EntityHandle r : roots)
        {
            if (!r.IsValid())
            {
                continue;
            }
            std::vector<ECS::EntityHandle> sub = DeleteEntitiesCommand::CollectSubtree(world, r);
            for (ECS::EntityHandle e : sub)
            {
                if (!e.IsValid())
                {
                    continue;
                }
                if (seen.insert(e.id).second)
                {
                    out.push_back(e);
                }
            }
        }

        std::sort(out.begin(), out.end(), [](const ECS::EntityHandle& a, const ECS::EntityHandle& b)
                  { return a.id < b.id; });
        return out;
    }

    std::string m_Name;

    std::unique_ptr<DeleteEntitiesCommand> m_Delete;

    std::vector<UI::Interaction::ItemId> m_SelectionBefore;
    UI::Interaction::ItemId m_AnchorBefore = 0;
    std::vector<UI::Interaction::ItemId> m_SelectionAfter;
    UI::Interaction::ItemId m_AnchorAfter = 0;

    ApplySelectionFn m_ApplySelection;
};

} // namespace GameEngine::Editor
