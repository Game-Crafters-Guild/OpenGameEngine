#pragma once

#include "ECS/Systems.h"
#include "ECS/ECS.h"
#include "ECS/ChangeFilter.h"
#include "Types/Types.h"

#include <array>
#include <mutex>
#include <span>
#include <vector>

namespace GameEngine { namespace Components { struct Transform; struct WorldTransform; struct Parent; } }

namespace GameEngine { namespace Engine::Renderer {

// Computes world-space transforms (WorldTransform) from local Transform + Parent
// hierarchy. See docs/TransformHierarchySystem.md for the component contract.
//
// Every pass opts back into disabled rows. A disabled entity keeps a world
// transform — the editor draws its gizmo and the inspector shows its position,
// and re-enabling it must not cost a frame of catch-up — and a disabled parent
// still has to appear in the CSR or its descendants would be re-rooted.
//
// Every entity without a Parent component resolves on the flat parallel pass:
// WorldTransform = Transform, only for changed Transform chunks, fanned across
// the job system. That holds whether or not hierarchies exist elsewhere in the
// world, so one parented entity never moves the rest of the world off it.
//
// Hierarchy members — entities with a Parent component, and the roots above
// them — also live in a cached parent->child CSR, rebuilt only after a Parent
// edit or a structural change. Each frame the dirty members are the
// Parent-holders whose local matrix changed (narrowed to exact entities by
// cached local matrices) and the hierarchy roots the flat pass just moved.
// Only the topmost of them are walked (a dirty node under a dirty ancestor is
// covered by the ancestor's walk), and those disjoint subtrees propagate in
// parallel when there are enough of them. A root without a Parent component
// is written by the flat pass and only read here, as its children's parent.
class TransformHierarchySystem : public ECS::ISystem {
public:
    TransformHierarchySystem();
    // Test seam: force the serial resolve even for flat worlds so a single
    // process can compare serial-vs-parallel output for determinism. Every
    // entity is a cached node, nothing takes the flat pass and subtrees
    // propagate one after another.
    explicit TransformHierarchySystem(bool forceSerial);

    const char* GetName() const override { return "TransformHierarchySystem"; }
    void Update(ECS::World& world, float32 deltaTime) override;

    // Lightweight diagnostics used by correctness/performance regression
    // tests and GE_HIER_PROFILE validation.
    size_t GetTopologyBuildCount() const { return m_TopologyBuildCount; }
    size_t GetLastPropagatedNodeCount() const { return m_LastPropagatedNodeCount; }

private:
    struct Node {
        ECS::EntityHandle entity{};
        const Components::Transform* local = nullptr;
        // Where propagation writes this node's world matrix; null when the
        // flat pass owns it.
        Components::WorldTransform* world = nullptr;
        // Where its children read it from (the same component either way).
        const Components::WorldTransform* worldRead = nullptr;
        ECS::EntityHandle parentEntity{};
        std::array<float32, 16> localSnapshot{};
        // A root without a Parent component: the flat pass writes its
        // WorldTransform, so propagation only reads it (world is null).
        bool flatOwned = false;
    };

    // Which entities the flat pass resolves: every entity (no hierarchy
    // exists), or only those without a Parent component.
    enum class FlatScope { AllEntities, Unparented };

    // Phase 0 (both paths): every entity with Transform gets a WorldTransform.
    void EnsureWorldTransforms(ECS::World& world);
    // Rebuild the cached entity index + CSR graph. Called only after a
    // structural change or Parent edit; Update then resolves every node.
    void RebuildTopology(ECS::World& world);
    // Detect exact Transform edits of Parent-holders inside Changed<Transform>
    // chunks, add the hierarchy roots the flat pass moved, and propagate the
    // topmost of them.
    void ResolveChangedSubtrees(ECS::World& world, double ensureMs);
    // Flat pass: WorldTransform = Transform for the scope's changed chunks,
    // fanned out over the job system (serial when the world has no pool).
    // Records the cached hierarchy roots whose world matrix it changed.
    void ResolveFlatParallel(ECS::World& world, FlatScope scope);
    bool HasParentColumnChange(ECS::World& world, uint64 entryVersion);
    size_t FindNodeIndex(ECS::EntityHandle entity) const;
    void AddNode(ECS::EntityHandle entity, const Components::Transform& local,
                 Components::WorldTransform& worldTransform, ECS::EntityHandle parent);
    // Appends a node for every Parent-holder's parent that has no Parent
    // component itself (a hierarchy root the flat pass owns).
    void AddFlatOwnedRoots(ECS::World& world);
    // True when an ancestor of `index` was marked dirty in this pass. Each
    // node's answer is memoized for the pass as the walk climbs, so the filter
    // costs O(nodes) in total however many dirty nodes share a long clean spine.
    bool HasDirtyAncestor(size_t index);

    // Whether a propagation pass must collect write grants for the
    // WorldTransforms it rewrites. The cached-node path writes through
    // pointers captured at topology rebuild, outside any query visit, so
    // nothing stamps those columns for it and Changed<WorldTransform>
    // consumers would never see a propagated move. The rebuild pass is the
    // exception: its node gather binds Write<WorldTransform> and its
    // stamp-at-visit has already covered every chunk.
    enum class StampCollection { Collect, AlreadyGranted };

    // Propagates each root's subtree (the roots' subtrees must be disjoint),
    // in parallel on the world's job system when there are enough of them,
    // then emits the rewritten entities to the dirty feed and, for Collect,
    // stamps their write grants in one batch.
    void PropagateRoots(ECS::World& world, std::span<const size_t> roots, StampCollection stamps);
    // Walks one subtree depth-first, rewriting world matrices whose bytes
    // change (Version bumped) and appending those entities to `changed`.
    // Touches only the subtree's nodes, so disjoint subtrees run concurrently.
    // Returns the number of nodes visited.
    size_t PropagateSubtree(size_t rootIndex, std::vector<size_t>& stack,
                            std::vector<ECS::EntityHandle>& changed);
    // One fork-join chunk of PropagateRoots: roots[begin, end) on chunk c's scratch.
    void PropagateRootChunk(std::span<const size_t> roots, size_t begin, size_t end, size_t chunk);

    bool m_ForceSerial = false; // test seam only; path selection is data-driven
    size_t m_LastFlatCount = 0; // entities processed by the last flat pass (profiling)
    size_t m_LastPropagatedNodeCount = 0;
    size_t m_TopologyBuildCount = 0;

    // Change-filter consumer state (ECS/ChangeFilter.h; the S0-proven gate,
    // now on the real per-(chunk x column) mechanism). Active only when
    // GE_ECS_CHANGE_FILTER is not 0. All gates are entry-sampled (design
    // M14) and paired with the world that produced them — thumbnail worlds
    // construct their own instances, but the pairing is load-bearing if one
    // instance ever serves two worlds.
    uint64 m_GateWorldId = 0;

    // Ensure-pass gate: entities needing WorldTransform can only appear via
    // a structural change (Transform add = archetype move), so the full
    // ensure query is skipped while structuralChangeVersion is unchanged —
    // this was the remaining 0.28 ms floor in the Step-0 A/B.
    std::size_t m_EnsureStructuralVersion = 0;
    bool m_EnsureGateValid = false;

    // Flat-path gate: Changed<Transform> chunk skip. Sound only while every
    // matched entity is a root (design C10/M1), so the gate resets whenever
    // the previous Update did not run the gated flat path — in particular on
    // the serial->flat transition, where an in-place Parent clear leaves the
    // Transform column unstamped but WorldTransform stale.
    ECS::ChangeGate m_FlatGate;
    bool m_FlatGateValid = false;

    // Hierarchy gates are separate: Parent changes invalidate topology while
    // Transform changes only produce dirty subtree candidates.
    ECS::ChangeGate m_ParentGate;
    bool m_ParentGateValid = false;
    ECS::ChangeGate m_TransformGate;
    bool m_TransformGateValid = false;
    std::size_t m_TopologyStructuralVersion = 0;
    bool m_TopologyValid = false;
    bool m_HasHierarchy = false;

    // Scratch buffers retained between frames to avoid per-frame heap allocations.
    // .clear() preserves capacity; after the first frame these never allocate.
    std::vector<ECS::EntityHandle> m_NeedWorldTransform;
    std::vector<Node> m_Nodes;
    // Direct entity-index -> node lookup. Entity indices are bounded to 20
    // bits, so even a million-entity scene needs only ~4 MiB at uint32/node.
    // The stored handle is still verified to reject a recycled generation.
    std::vector<uint32> m_NodeIndexByEntityIndex;
    std::vector<size_t> m_ResolvedParent; // resolved parent node index per node (SIZE_MAX = root)
    // CSR adjacency: children of node i are m_ChildIndices[m_ChildOffsets[i]..m_ChildOffsets[i+1]).
    std::vector<size_t> m_ChildOffsets;
    std::vector<size_t> m_ChildIndices;
    std::vector<size_t> m_Roots;
    std::vector<size_t> m_DirtyNodes;
    std::vector<size_t> m_TraversalStack;
    // Entities whose WorldTransform this pass actually rewrote, staged for one
    // batched write-grant stamp. Filled and drained inside a single Update.
    std::vector<ECS::EntityHandle> m_StampScratch;
    std::vector<uint8_t> m_TopologyState;
    // Hierarchy roots the flat pass moved this frame (node indices), appended
    // under the mutex once per chunk that moved any.
    std::vector<size_t> m_ChangedFlatRoots;
    std::mutex m_ChangedFlatRootsMutex;
    // Topmost-dirty filter: the pass that last marked each node dirty, and
    // the pass that last resolved whether the node or an ancestor is dirty
    // (the answer in m_CoveredByNode), with the climb's scratch path.
    std::vector<uint32> m_DirtyPassByNode;
    std::vector<uint32> m_CoveredPassByNode;
    std::vector<uint8_t> m_CoveredByNode;
    std::vector<size_t> m_CoverPath;
    uint32 m_DirtyPass = 0;
    // Parallel propagation: one stack and one changed list per fork-join chunk.
    std::vector<std::vector<size_t>> m_ChunkStacks;
    std::vector<std::vector<ECS::EntityHandle>> m_ChunkChanged;
    std::vector<size_t> m_ChunkVisited;
};

} } // namespace GameEngine::Engine::Renderer
