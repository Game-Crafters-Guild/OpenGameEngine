#pragma once

#include "Components/Hierarchy.h"
#include "ECS/Query.h"
#include "ECS/RelationIndex.h"
#include "ECS/World.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace GameEngine {
namespace Components {

// ----------------------------------------------------------------------------
// Hierarchy query helpers — entity-level scan of Parent components.
//
// These walk every entity that carries a Parent component and filter by the
// requested relation. No archetype-per-parent trick, so they compose with the
// existing archetype graph unchanged.
//
// Two flavors:
// - Stateless overloads: each call builds a fresh scan. O(parented-entities)
//   per call. Fine for ad-hoc lookups.
// - Indexed overloads (take a `ParentRelationIndex&`): amortized O(subtree)
//   per call, rebuild on structural change. Fine for hot paths with many
//   queries per frame. Crossover at ~20 queries per rebuild — see
//   HierarchyBenchmark.RelationIndex_vs_Scan.
//
// For deep traversal-heavy workloads, TransformHierarchySystem already builds
// a CSR adjacency graph — prefer that path over repeated DescendantsOf calls.
// ----------------------------------------------------------------------------

// Named RelationIndex instantiation for the Parent-of relation. Callers
// hold one of these as a long-lived member and pass it to the indexed
// helpers below.
struct ParentOfTag {};
using ParentRelationIndex = ECS::RelationIndex<ParentOfTag, Parent, &Parent::parent>;

inline void ChildrenOf(ECS::World& world, ECS::EntityHandle parent,
                       std::vector<ECS::EntityHandle>& out) {
    out.clear();
    if (!parent.IsValid()) return;
    // Hierarchy answers cover the whole scene, disabled branches included —
    // the editor lists them and scene IO writes them.
    world.Query<ECS::Read<Parent>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle e, const Parent& p) {
            if (p.parent == parent) out.push_back(e);
        });
}

inline std::vector<ECS::EntityHandle> ChildrenOf(ECS::World& world,
                                                 ECS::EntityHandle parent) {
    std::vector<ECS::EntityHandle> out;
    ChildrenOf(world, parent, out);
    return out;
}

// Indexed variant — O(children-of-parent) hot, O(parented-entities) on rebuild.
inline void ChildrenOf(ECS::World& world, ECS::EntityHandle parent,
                       ParentRelationIndex& index,
                       std::vector<ECS::EntityHandle>& out) {
    out.clear();
    if (!parent.IsValid()) return;
    const auto* sources = index.GetSources(world, parent);
    if (sources) out = *sources;
}

// Breadth-first descendant walk. Snapshots every (child, parent) edge once,
// grouped by parent, then expands level-by-level via hash lookups —
// O(parented-entities + subtree), not the former O(subtree * parented-entities)
// nested rescan (which made one 10k-child lookup cost ~300ms).
// Output order is BFS from the seed downward. The seed itself is excluded
// from the result.
// Cycle-safe: a visited set prevents infinite loops when Parent chains form cycles.
inline void DescendantsOf(ECS::World& world, ECS::EntityHandle ancestor,
                          std::vector<ECS::EntityHandle>& out) {
    out.clear();
    if (!ancestor.IsValid()) return;

    std::unordered_map<ECS::EntityHandle, std::vector<ECS::EntityHandle>,
                       ECS::EntityHandleHash>
        childrenByParent;
    world.Query<ECS::Read<Parent>>().IncludeDisabled().Each(
        [&](ECS::EntityHandle e, const Parent& p) {
            if (p.parent.IsValid()) childrenByParent[p.parent].push_back(e);
        });

    std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash> visited;
    visited.insert(ancestor);

    std::vector<ECS::EntityHandle> frontier;
    frontier.push_back(ancestor);
    while (!frontier.empty()) {
        std::vector<ECS::EntityHandle> next;
        for (ECS::EntityHandle current : frontier) {
            const auto it = childrenByParent.find(current);
            if (it == childrenByParent.end()) continue;
            for (ECS::EntityHandle child : it->second) {
                if (visited.insert(child).second) {
                    out.push_back(child);
                    next.push_back(child);
                }
            }
        }
        frontier.swap(next);
    }
}

inline std::vector<ECS::EntityHandle> DescendantsOf(ECS::World& world,
                                                    ECS::EntityHandle ancestor) {
    std::vector<ECS::EntityHandle> out;
    DescendantsOf(world, ancestor, out);
    return out;
}

// Indexed BFS variant — each frontier step is an O(1) index lookup instead of
// a scan over every parented entity. Amortized O(subtree size) per call.
inline void DescendantsOf(ECS::World& world, ECS::EntityHandle ancestor,
                          ParentRelationIndex& index,
                          std::vector<ECS::EntityHandle>& out) {
    out.clear();
    if (!ancestor.IsValid()) return;

    std::unordered_set<ECS::EntityHandle, ECS::EntityHandleHash> visited;
    visited.insert(ancestor);

    std::vector<ECS::EntityHandle> frontier;
    frontier.push_back(ancestor);
    while (!frontier.empty()) {
        std::vector<ECS::EntityHandle> next;
        for (ECS::EntityHandle current : frontier) {
            const auto* kids = index.GetSources(world, current);
            if (!kids) continue;
            for (ECS::EntityHandle child : *kids) {
                if (visited.insert(child).second) {
                    out.push_back(child);
                    next.push_back(child);
                }
            }
        }
        frontier.swap(next);
    }
}

// Practical cap on hierarchy depth. Any real scene deeper than this is a
// design smell — UI panels nest ~10 deep, scene-graph rigs top out around 30.
// The cap bounds IsDescendantOf so a broken chain on a million-entity world
// doesn't take a million `worldMutex` shared_lock acquisitions.
inline constexpr std::size_t kMaxHierarchyDepth = 256;

// Walks from `entity` up its Parent chain looking for `ancestor`. Cycle-safe:
// bounded by `kMaxHierarchyDepth` so pathological chains cannot loop and a
// broken chain on a huge world terminates quickly. Returns false if
// `ancestor` no longer exists in the world (even if stale Parent handles
// still reference it by bits).
inline bool IsDescendantOf(ECS::World& world, ECS::EntityHandle entity,
                           ECS::EntityHandle ancestor) {
    if (!entity.IsValid() || !ancestor.IsValid() || entity == ancestor) return false;
    if (!world.IsValid(ancestor)) return false; // Destroyed ancestor — treat chain as broken.
    ECS::EntityHandle current = entity;
    for (std::size_t step = 0; step < kMaxHierarchyDepth; ++step) {
        const Parent* p = world.GetComponent<Parent>(current);
        if (!p || !p->parent.IsValid()) return false;
        if (p->parent == ancestor) return true;
        current = p->parent;
    }
    return false;
}

} // namespace Components
} // namespace GameEngine
