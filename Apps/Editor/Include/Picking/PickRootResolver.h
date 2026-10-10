// Scene View pick-target resolution: maps a raycast hit to the entity a
// click should select, honoring model-instance boundaries (the engine's
// analog of Unity's "outermost prefab root" pick).
#pragma once

#include <vector>

#include "ECS/Entity.h"

namespace GameEngine::ECS
{
class World;
}

namespace GameEngine::Editor::Picking
{
struct PickHit;

// Resolve the entity a root-mode pick selects: the topmost contiguous
// ancestor that belongs to the same model instance as `entity` — mesh
// ancestors referencing the same model asset, plus the Transform-only
// container/helper nodes ModelEntityFactory creates inside one instance.
// Plain grouping nodes whose subtree renders anything from a different asset
// are never crossed, so deep scene hierarchies (City > Buildings > Room)
// don't swallow the pick. Entities without an asset-backed MeshRenderer
// (lights, probes, primitives, plugin pick-root components) resolve to
// themselves.
ECS::EntityHandle ResolvePickRoot(ECS::World& world, ECS::EntityHandle entity);

// Ordered click-through candidates from a distance-sorted, pickability-
// filtered hit list. Root mode prepends the nearest hit's instance root:
// [root, nearest exact hit, deeper exact hits...]; exact mode is just the
// exact hit entities nearest-to-farthest. Deduplicated, order preserved.
void BuildPickCandidates(ECS::World& world,
                         const std::vector<PickHit>& sortedHits,
                         bool exactPick,
                         std::vector<ECS::EntityHandle>& outCandidates);
} // namespace GameEngine::Editor::Picking
