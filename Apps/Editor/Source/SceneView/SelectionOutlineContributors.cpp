#include "SceneView/SelectionOutlineContributors.h"

#include "ECS/Entity.h"
#include "ECS/World.h"

#include <algorithm>

namespace GameEngine::Editor
{
namespace
{
bool OutlineListContains(std::span<const ECS::EntityHandle> entities, ECS::EntityHandle entity)
{
    return std::find(entities.begin(), entities.end(), entity) != entities.end();
}

bool DrawsAsActiveInHierarchy(ECS::World& world, ECS::EntityHandle entity)
{
    return world.IsValid(entity) && ECS::Entity(&world, entity).IsEnabledInHierarchy();
}
} // namespace

void CollectSelectionOutlineContributors(ECS::World& world, const SelectionOutlineSources& sources,
                                         std::vector<ECS::EntityHandle>& outContributors)
{
    outContributors.clear();
    outContributors.reserve(sources.Selected.size() + sources.SelectedDescendants.size() +
                            sources.HoveredDescendants.size() + 1u);
    if (sources.IncludeSelected)
    {
        for (const ECS::EntityHandle entity : sources.Selected)
            if (DrawsAsActiveInHierarchy(world, entity))
                outContributors.push_back(entity);
        for (const ECS::EntityHandle descendant : sources.SelectedDescendants)
            if (!OutlineListContains(sources.Selected, descendant) && DrawsAsActiveInHierarchy(world, descendant))
                outContributors.push_back(descendant);
    }
    if (!sources.Hovered.IsValid())
        return;
    const auto listedAsSelected = [&](ECS::EntityHandle entity)
    { return sources.IncludeSelected && OutlineListContains(sources.Selected, entity); };
    if (!listedAsSelected(sources.Hovered) && DrawsAsActiveInHierarchy(world, sources.Hovered))
        outContributors.push_back(sources.Hovered);
    for (const ECS::EntityHandle descendant : sources.HoveredDescendants)
        if (!listedAsSelected(descendant) && DrawsAsActiveInHierarchy(world, descendant))
            outContributors.push_back(descendant);
}

} // namespace GameEngine::Editor
