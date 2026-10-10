#include "Picking/PickRootResolver.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "Components/Hierarchy.h"
#include "Components/Rendering/MeshRenderer.h"
#include "ECS/ECS.h"
#include "ECS/Query.h"
#include "Editor/Entities/EditorComponentTraits.h"
#include "Picking/MeshPickingService.h"

namespace GameEngine::Editor::Picking
{

namespace
{

using ChildrenByParent =
    std::unordered_map<ECS::EntityHandle, std::vector<ECS::EntityHandle>, ECS::EntityHandleHash>;

// Snapshot every (parent -> children) edge in one pass. O(parented-entities),
// so the resolve walk builds it lazily, at most once, and only when it
// actually reaches a container node. When a model-instance-root marker
// component exists (planned with the prefab/blueprint system), this scan and
// the subtree check below disappear in favor of an O(depth) climb.
void BuildChildrenMap(ECS::World& world, ChildrenByParent& out)
{
    world.Query<ECS::Read<Components::Parent>>().IncludeDisabled().Each(
        [&out](ECS::EntityHandle e, const Components::Parent& p)
        {
            if (p.parent.IsValid())
                out[p.parent].push_back(e);
        });
}

// True when nothing in `root`'s subtree renders an asset other than `model`.
// Lets the root walk climb through Transform-only container and helper nodes
// that belong to one model instance, while refusing to climb into grouping
// nodes that also contain other content.
bool SubtreeRendersSingleModel(ECS::World& world,
                               const ChildrenByParent& children,
                               ECS::EntityHandle root,
                               const GUID& model)
{
    // Reused across calls so hover/click resolves don't heap-allocate the
    // traversal stack each time (picking runs on the main thread; thread_local
    // keeps any future off-thread use safe).
    thread_local std::vector<ECS::EntityHandle> frontier;
    frontier.clear();
    frontier.push_back(root);
    while (!frontier.empty())
    {
        const ECS::EntityHandle current = frontier.back();
        frontier.pop_back();

        // Components registered as their own pick target (plugin traits) stop
        // the model-instance grouping: their entity never folds into a root.
        for (const auto& [typeId, traits] : EditorComponentTraitsRegistry::Get().Snapshot())
        {
            if (traits.IsPickInstanceRoot && world.HasComponent(current, typeId))
                return false;
        }
        const auto* mr = world.GetComponent<Components::MeshRenderer>(current);
        if (mr && !(mr->modelAssetGuid.ToGuid() == model))
            return false;

        const auto it = children.find(current);
        if (it != children.end())
            frontier.insert(frontier.end(), it->second.begin(), it->second.end());
    }
    return true;
}

} // namespace

ECS::EntityHandle ResolvePickRoot(ECS::World& world, ECS::EntityHandle entity)
{
    if (!entity.IsValid() || !world.IsValid(entity))
        return entity;

    const auto* mr = world.GetComponent<Components::MeshRenderer>(entity);
    if (!mr)
        return entity;

    // Primitives and unassigned renderers carry no instance identity; a null
    // GUID would merge unrelated hand-placed primitives, so pick them exactly.
    const GUID model = mr->modelAssetGuid.ToGuid();
    if (model.IsNull())
        return entity;

    // Reused across resolves: clear() keeps the bucket array, so repeat
    // resolves avoid the map's rehash allocations (per-key child vectors are
    // still rebuilt — acceptable, the map is only built when the walk reaches
    // a container node at all).
    thread_local ChildrenByParent children;
    bool childrenBuilt = false;

    for (;;)
    {
        const auto* parentComp = world.GetComponent<Components::Parent>(entity);
        if (!parentComp || !parentComp->parent.IsValid() || !world.IsValid(parentComp->parent))
            break;

        const ECS::EntityHandle parent = parentComp->parent;
        const auto* parentMr = world.GetComponent<Components::MeshRenderer>(parent);
        bool sameInstance = false;
        if (parentMr)
        {
            sameInstance = (parentMr->modelAssetGuid.ToGuid() == model);
        }
        else
        {
            if (!childrenBuilt)
            {
                children.clear();
                BuildChildrenMap(world, children);
                childrenBuilt = true;
            }
            sameInstance = SubtreeRendersSingleModel(world, children, parent, model);
        }
        if (!sameInstance)
            break;
        entity = parent;
    }
    return entity;
}

void BuildPickCandidates(ECS::World& world,
                         const std::vector<PickHit>& sortedHits,
                         bool exactPick,
                         std::vector<ECS::EntityHandle>& outCandidates)
{
    outCandidates.clear();
    if (sortedHits.empty())
        return;

    outCandidates.reserve(sortedHits.size() + 1);
    if (!exactPick)
    {
        const ECS::EntityHandle root = ResolvePickRoot(world, sortedHits.front().Entity);
        if (root.IsValid())
            outCandidates.push_back(root);
    }
    for (const PickHit& hit : sortedHits)
    {
        if (std::find(outCandidates.begin(), outCandidates.end(), hit.Entity) == outCandidates.end())
            outCandidates.push_back(hit.Entity);
    }
}

} // namespace GameEngine::Editor::Picking
