#pragma once

#include "ECS/Entity.h"
#include "ECS/Query.h"
#include "ECS/World.h"

#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace GameEngine::ECS {

// ----------------------------------------------------------------------------
// RelationIndex
//
// Caches `target -> [sources...]` for a component that points from a source
// entity to a target entity. Lookup is O(children-of-target) instead of
// O(entities-with-component) as in the naive scan.
//
// Cache invalidation uses the World's `structuralChangeVersion`: any
// structural change (entity create/destroy, component add/remove, archetype
// compaction) bumps the counter, triggering a lazy rebuild on the next
// GetSources() call. This over-invalidates (a bystander entity create
// invalidates an unrelated ParentOf index), but in practice structural
// changes are frame-scoped while queries run every frame, so the amortized
// cost approaches one rebuild per frame.
//
// A future Phase 2 may add per-op incremental updates via an entity-aware
// hook API. Phase 1 deliberately avoids touching the hook surface.
//
// Usage:
//   struct ParentOf {};                  // relation tag (empty struct)
//   using ParentIndex = RelationIndex<ParentOf, Components::Parent, &Components::Parent::parent>;
//
//   ParentIndex idx;
//   const auto* children = idx.GetSources(world, targetEntity);
//   if (children) for (auto c : *children) { ... }
//
// Thread safety:
// - Multiple readers may call GetSources concurrently.
// - Rebuild acquires an internal unique mutex; only one thread rebuilds at a
//   time. The caller of GetSources must have a coherent view of `world`
//   (i.e. no structural mutation in parallel) — same contract as iterating a
//   Query manually.
// ----------------------------------------------------------------------------

template <class RelationTag, class Component, EntityHandle Component::*TargetMember>
class RelationIndex {
  public:
    // Returns the list of source entities (children) for `target`, rebuilding
    // the index first if the world's structural version has advanced.
    // Returns nullptr if `target` has no sources.
    const std::vector<EntityHandle>* GetSources(World& world, EntityHandle target) {
        EnsureFresh(world);
        std::shared_lock read(m_Mutex);
        auto it = m_ChildrenByTarget.find(target);
        return it != m_ChildrenByTarget.end() ? &it->second : nullptr;
    }

    // Count entities that point at `target`. Cheaper than fetching the vector
    // when the caller only needs the size.
    std::size_t CountSources(World& world, EntityHandle target) {
        const auto* v = GetSources(world, target);
        return v ? v->size() : 0u;
    }

    // Force invalidation on next GetSources. Primarily for tests.
    void Invalidate() {
        std::unique_lock write(m_Mutex);
        m_CachedVersion = kUninitialized;
        m_ChildrenByTarget.clear();
    }

  private:
    static constexpr std::size_t kUninitialized = ~std::size_t{0};

    void EnsureFresh(World& world) {
        const std::size_t worldVersion = world.GetStructuralChangeVersion();
        {
            std::shared_lock read(m_Mutex);
            if (m_CachedVersion == worldVersion) return; // hot path
        }
        // Slow path — rebuild under the unique lock.
        std::unique_lock write(m_Mutex);
        // Re-check: another thread may have rebuilt while we waited.
        if (m_CachedVersion == worldVersion) return;
        Rebuild(world);
        m_CachedVersion = worldVersion;
    }

    void Rebuild(World& world) {
        m_ChildrenByTarget.clear();
        // A relation is a fact about the world, not a fact about what is
        // running: a disabled entity still has the parent it was authored with,
        // and the editor's hierarchy is built from this index.
        world.Query<Read<Component>>().IncludeDisabled().Each(
            [this](EntityHandle e, const Component& c) {
                EntityHandle target = c.*TargetMember;
                if (target.IsValid()) {
                    m_ChildrenByTarget[target].push_back(e);
                }
            });
    }

    mutable std::shared_mutex m_Mutex;
    std::unordered_map<EntityHandle, std::vector<EntityHandle>, EntityHandleHash> m_ChildrenByTarget;
    std::size_t m_CachedVersion = kUninitialized;
};

} // namespace GameEngine::ECS
