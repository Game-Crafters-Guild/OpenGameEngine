#pragma once

#include "ECS/ECS.h"
#include "Editor/Hierarchy/HierarchyEnableState.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace GameEngine
{
class TreeChangeTrackingProvider;
class TreeView;
class UIElement;
} // namespace GameEngine

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// Keeps the Hierarchy's rows showing their entities' enable state (DescribeEntityActivity). A row
/// shows its entity's state when the tree binds it (Present). The derived state lands with the
/// hierarchy pass a frame after the switch that caused it, so when the world's structure has changed
/// Refresh marks, on the tree's provider, every id of a row the tree holds whose entity's state moved
/// since the row was given it; the tree rebinds those rows. It remembers tree ids, never rows (the
/// tree destroys pooled rows when its viewport shrinks), and forgets the ids of rows the tree no
/// longer holds, so each refresh costs the rows held, not every id ever shown.
class HierarchyRowActivity
{
  public:
    /// Shows `entity`'s state on `row`, the row the tree has just bound to `treeId`.
    void Present(ECS::World& world, ECS::EntityHandle entity, std::uint64_t treeId, UIElement& row);

    /// When the world's structural version has moved since the last call, marks on `provider` the ids
    /// of the rows `tree` holds whose entity's state differs from the one last shown, and forgets the
    /// ids of rows it no longer holds. Does nothing otherwise.
    void Refresh(ECS::World& world, const TreeView& tree, TreeChangeTrackingProvider& provider);

  private:
    struct Shown
    {
        ECS::EntityHandle Entity{};
        EntityActivity Activity{};
        std::uint64_t HeldAtRefresh = 0; // the last refresh that found the id's row held
    };

    std::unordered_map<std::uint64_t, Shown> m_Shown;
    std::vector<std::uint64_t> m_Held;  // reused across refreshes
    std::vector<std::uint64_t> m_Moved; // reused across refreshes
    std::uint64_t m_Refreshes = 0;
    std::size_t m_StructuralVersion = 0;
    bool m_HasStructuralVersion = false;
};

} // namespace GameEngine::Editor
