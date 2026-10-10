#pragma once

#include <span>
#include <vector>

#include "ECS/ECS.h"

namespace GameEngine::ECS
{
class World;
} // namespace GameEngine::ECS

namespace GameEngine::Editor
{

/// What the Scene View's selection outline is drawn from.
struct SelectionOutlineSources
{
    std::span<const ECS::EntityHandle> Selected;
    std::span<const ECS::EntityHandle> SelectedDescendants;
    ECS::EntityHandle Hovered{};
    std::span<const ECS::EntityHandle> HoveredDescendants;
    /// False while selection outlines are hidden; the hover still outlines.
    bool IncludeSelected = true;
};

/// Fills outContributors with the entities the selection outline draws, in order: the selection
/// and its descendants (when IncludeSelected), then the hovered entity and its descendants,
/// skipping those already listed as selected. An entity that is not active in the hierarchy
/// (switched off, or under a switched-off parent) renders nothing, so it is left out.
void CollectSelectionOutlineContributors(ECS::World& world, const SelectionOutlineSources& sources,
                                         std::vector<ECS::EntityHandle>& outContributors);

} // namespace GameEngine::Editor
