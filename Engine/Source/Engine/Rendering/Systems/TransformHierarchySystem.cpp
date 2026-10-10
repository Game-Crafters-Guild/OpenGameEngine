#include "ECSModules/Rendering/Systems/TransformHierarchySystem.h"

#include "ECS/World.h"
#include "ECS/Query.h"
#include "ECS/ECSTemplates.h"
#include "Components/Transform.h"
#include "Components/TransformDirtyFeed.h"
#include "Components/Hierarchy.h"
#include "Logger/Logger.h"
#include "JobSystem/JobCounter.h"
#include "JobSystem/WorkStealingThreadPool.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace GameEngine { namespace Engine::Renderer {

using GameEngine::Components::Transform;
using GameEngine::Components::WorldTransform;
using GameEngine::Components::Parent;

namespace {

// Multiply 4x4 column-major matrices: out = a * b
inline void MultiplyColumnMajor4x4(const float32* a, const float32* b, float32* out) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            const int o = col * 4 + row;
            out[o] = a[0 * 4 + row] * b[col * 4 + 0]
                   + a[1 * 4 + row] * b[col * 4 + 1]
                   + a[2 * 4 + row] * b[col * 4 + 2]
                   + a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
}

// Compute WorldTransform = Transform for one entity, bumping Version only when
// the bytes actually change. This is the per-entity root computation shared by
// the cached hierarchy (for root nodes) and the flat parallel path; keeping it in one
// place is what guarantees bitwise-identical output across the two paths.
// Returns true when the bytes changed (Version bumped) — the caller owns the
// matching dirty-feed emission (per-entity on the hierarchy path, chunk-batched
// on the flat parallel path), so a bump can never outrun its feed entry.
inline bool WriteRootWorldTransform(const float32* local, WorldTransform& out) {
    if (std::memcmp(out.matrix, local, sizeof(float32) * 16) != 0) {
        std::memcpy(out.matrix, local, sizeof(float32) * 16);
        ++out.Version;
        return true;
    }
    return false;
}

// GE_HIER_PROFILE=1 logs a per-phase decomposition of Update() every
// kProfileLogInterval active frames. The cached path reports entity scanning,
// direct-index construction, CSR rebuilding, and subtree propagation separately.
const bool kProfileEnabled = [] {
    const char* v = std::getenv("GE_HIER_PROFILE");
    return v && v[0] == '1';
}();
constexpr uint32 kProfileLogInterval = 120u;

// Dirty subtrees propagate on the job system from this many up; below it the
// fork costs more than the walk. Subtrees are handed out in chunks of at least
// kMinRootsPerChunk.
constexpr size_t kParallelMinRoots = 256;
constexpr size_t kMinRootsPerChunk = 64;

using ProfileClock = std::chrono::steady_clock;
inline ProfileClock::time_point Mark() { return ProfileClock::now(); }
inline double DeltaMs(ProfileClock::time_point a, ProfileClock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

struct HierarchyProfiler {
    double ensureMs = 0.0, scanMs = 0.0, indexMs = 0.0, topologyMs = 0.0, propagateMs = 0.0;
    uint32 frames = 0;
    size_t lastNodeCount = 0, lastRootCount = 0, lastEdgeCount = 0;

    void Accumulate(double ensure, double scan, double index, double topology, double propagate,
                    size_t nodes, size_t roots, size_t edges) {
        ensureMs += ensure; scanMs += scan; indexMs += index; topologyMs += topology; propagateMs += propagate;
        lastNodeCount = nodes; lastRootCount = roots; lastEdgeCount = edges;
        if (++frames >= kProfileLogInterval) Log();
    }
    void Log() {
        if (frames == 0) return;
        const double f = static_cast<double>(frames);
        Logger::Log::Info(
            "[HierProfile] cached nodes={} roots={} edges={} | ensure={:.3f} scan={:.3f} "
            "index={:.3f} topology={:.3f} propagate={:.3f} total={:.3f} ms (avg/{} frames)",
            lastNodeCount, lastRootCount, lastEdgeCount,
            ensureMs / f, scanMs / f, indexMs / f, topologyMs / f, propagateMs / f,
            (ensureMs + scanMs + indexMs + topologyMs + propagateMs) / f, frames);
        ensureMs = scanMs = indexMs = topologyMs = propagateMs = 0.0;
        frames = 0;
    }
};

struct FlatProfiler {
    double ensureMs = 0.0, detectMs = 0.0, computeMs = 0.0;
    uint32 frames = 0;
    size_t lastNodeCount = 0;

    void Accumulate(double ensure, double detect, double compute, size_t nodes) {
        ensureMs += ensure; detectMs += detect; computeMs += compute;
        lastNodeCount = nodes;
        if (++frames >= kProfileLogInterval) Log();
    }
    void Log() {
        if (frames == 0) return;
        const double f = static_cast<double>(frames);
        Logger::Log::Info(
            "[HierProfile] flat nodes={} | ensure={:.3f} detect={:.3f} compute={:.3f} "
            "total={:.3f} ms (avg/{} frames)",
            lastNodeCount, ensureMs / f, detectMs / f, computeMs / f,
            (ensureMs + detectMs + computeMs) / f, frames);
        ensureMs = detectMs = computeMs = 0.0;
        frames = 0;
    }
};

HierarchyProfiler g_hierarchyProfiler;
FlatProfiler g_flatProfiler;

// True if any entity carries a Parent whose handle is set. The flat path is
// only safe when this is false (every entity is a genuine root). A set-but-
// dangling parent handle conservatively returns true, routing to the cached
// hierarchy resolve which treats an unresolvable parent as a root.
bool HasAnyValidParent(ECS::World& world) {
    using namespace GameEngine::ECS;
    bool found = false;
    world.Query<Read<Parent>>().IncludeDisabled().Each([&](EntityHandle, const Parent& p) {
        if (p.parent.IsValid()) found = true;
    });
    return found;
}

} // anonymous namespace

TransformHierarchySystem::TransformHierarchySystem() = default;

TransformHierarchySystem::TransformHierarchySystem(bool forceSerial)
    : m_ForceSerial(forceSerial) {}

void TransformHierarchySystem::Update(ECS::World& world, float32 /*deltaTime*/) {
    const bool filterOn = ECS::ChangeFilter::Enabled();

    // Gates pair with the world whose counters produced them.
    if (m_GateWorldId != world.GetWorldId()) {
        m_GateWorldId = world.GetWorldId();
        m_EnsureGateValid = false;
        m_FlatGateValid = false;
        m_ParentGateValid = false;
        m_TransformGateValid = false;
        m_TopologyValid = false;
        m_HasHierarchy = false;
        m_Nodes.clear();
    }

    const auto tStart = Mark();
    // Ensure gate: an entity that needs a WorldTransform can only appear via
    // a structural change (gaining Transform is an archetype move), so the
    // full ensure query is skipped while structuralChangeVersion is stable —
    // the remaining 0.28 ms floor from the Step-0 A/B. Entry-sampled: the
    // adds the ensure pass itself performs can bump the version, so cache the
    // post-pass value and avoid an extra empty ensure on the next frame.
    const std::size_t structuralVersion = world.GetStructuralChangeVersion();
    if (!filterOn || !m_EnsureGateValid || m_EnsureStructuralVersion != structuralVersion) {
        EnsureWorldTransforms(world);
        m_EnsureStructuralVersion = world.GetStructuralChangeVersion();
        m_EnsureGateValid = true;
    }
    const double ensureMs = DeltaMs(tStart, Mark());

    const std::size_t currentStructuralVersion = world.GetStructuralChangeVersion();
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    const bool structuralChange =
        !m_TopologyValid || m_TopologyStructuralVersion != currentStructuralVersion;
    const bool parentChange =
        !structuralChange && HasParentColumnChange(world, entryVersion);

    const bool rebuilt = structuralChange || parentChange;
    if (rebuilt) {
        RebuildTopology(world);
        m_TopologyStructuralVersion = world.GetStructuralChangeVersion();
        m_TopologyValid = true;

        // The rebuild snapshots all Parent and Transform values present at
        // entry. Any later stamp compares greater on the next update.
        m_ParentGate.LastRunVersion = entryVersion;
        m_ParentGateValid = true;
        m_TransformGate.LastRunVersion = entryVersion;
        m_TransformGateValid = true;
    }

    if (!m_HasHierarchy) {
        const auto tCompute = Mark();
        ResolveFlatParallel(world, FlatScope::AllEntities);
        if (rebuilt)
            m_LastPropagatedNodeCount = m_LastFlatCount;
        if (kProfileEnabled) {
            g_flatProfiler.Accumulate(ensureMs, 0.0, DeltaMs(tCompute, Mark()), m_LastFlatCount);
        }
        return;
    }

    // Roots without a Parent component take the flat pass first: their
    // children read the world matrix it writes.
    m_ChangedFlatRoots.clear();
    if (!m_ForceSerial)
        ResolveFlatParallel(world, FlatScope::Unparented);

    if (rebuilt) {
        const auto tPropagateStart = Mark();
        PropagateRoots(world, m_Roots, StampCollection::AlreadyGranted);
        if (kProfileEnabled) {
            g_hierarchyProfiler.Accumulate(ensureMs, 0.0, 0.0, 0.0, DeltaMs(tPropagateStart, Mark()),
                                           m_Nodes.size(), m_Roots.size(), m_ChildIndices.size());
        }
        return;
    }

    ResolveChangedSubtrees(world, ensureMs);
}

void TransformHierarchySystem::EnsureWorldTransforms(ECS::World& world) {
    using namespace GameEngine::ECS;

    // IMPORTANT: Do not mutate archetypes while iterating query results.
    // Adding a component can move entities between archetypes and invalidate
    // references captured from the query (e.g. `const Transform& local`).
    m_NeedWorldTransform.clear();
    {
        auto qEnsure = world.Query<Read<Transform>, Optional<WorldTransform>>();
        qEnsure.IncludeDisabled();
        qEnsure.Each([&](EntityHandle e, const Transform& /*local*/, const WorldTransform* wt) {
            if (!wt)
                m_NeedWorldTransform.push_back(e);
        });
    }
    for (EntityHandle e : m_NeedWorldTransform)
    {
        if (!world.IsValid(e))
            continue;
        WorldTransform init{};
        world.AddComponentImmediate<WorldTransform>(e, init);
    }
}

void TransformHierarchySystem::ResolveFlatParallel(ECS::World& world, FlatScope scope) {
    using namespace GameEngine::ECS;

    auto q = world.Query<Read<Transform>, Write<WorldTransform>>();
    q.IncludeDisabled();
    if (scope == FlatScope::Unparented)
        q.Without<Parent>();

    // Changed<Transform> chunk skip (the S0-proven gate on the real
    // per-column mechanism). Sound here only because every entity on this
    // pass is a root — WorldTransform derives from this chunk's Transform
    // alone (design C10/M1). Entry-sample the version BEFORE iterating and
    // write that sample back as the next gate (design M14): a writer
    // stamping after the sample compares greater next run. The query's own
    // WorldTransform write-visit stamps never re-trigger the gate — it
    // filters the Transform column only (C14). The scope changes only with a
    // topology rebuild, which invalidates the gate.
    const bool filterOn = ChangeFilter::Enabled();
    uint64 entryVersion = 0;
    if (filterOn) {
        if (!m_FlatGateValid) {
            m_FlatGate.LastRunVersion = 0; // see everything once (path transition / first run)
        }
        entryVersion = world.GetGlobalSystemVersion();
        q.Changed<Transform>(m_FlatGate);
    }

    // Per-chunk task; each entity writes only its own WorldTransform slot, so
    // chunks never share a write target and no synchronization is needed.
    // ParallelBatchEach fans out via jobSystem->Run + participating Wait, and
    // falls back to a serial BatchEach when the world has no pool.
    //
    // Dirty-feed emission (§5 P2): changed entities are staged in a
    // thread-local scratch and appended once per chunk — the feed lock is
    // taken only for chunks containing actual movers, never per entity. The
    // scratch has no cross-frame or cross-world state (filled and drained
    // within one callback), so world churn can't dangle it. A changed entity
    // that is a cached hierarchy root is recorded the same way, so its
    // subtree propagates this frame.
    const bool feedOn = world.IsComponentDirtyFeedEnabledFor(
        ECS::GetComponentTypeId<WorldTransform>());
    const bool recordRoots = scope == FlatScope::Unparented;
    std::atomic<size_t> total{0};
    q.ParallelBatchEach([this, &total, &world, feedOn, recordRoots](const ECS::EntityHandle* entities,
                                                                    const Transform* locals,
                                                                    WorldTransform* worlds,
                                                                    std::size_t chunkCount) {
        thread_local std::vector<ECS::EntityHandle> tl_ChangedScratch;
        thread_local std::vector<size_t> tl_ChangedRoots;
        tl_ChangedScratch.clear();
        tl_ChangedRoots.clear();
        for (std::size_t i = 0; i < chunkCount; ++i) {
            if (!WriteRootWorldTransform(locals[i].matrix, worlds[i]))
                continue;
            if (feedOn)
                tl_ChangedScratch.push_back(entities[i]);
            if (recordRoots) {
                const size_t node = FindNodeIndex(entities[i]);
                if (node != SIZE_MAX)
                    tl_ChangedRoots.push_back(node);
            }
        }
        if (!tl_ChangedScratch.empty())
            world.EmitComponentDirtyBatch(ECS::GetComponentTypeId<WorldTransform>(),
                                          tl_ChangedScratch.data(), tl_ChangedScratch.size());
        if (!tl_ChangedRoots.empty()) {
            std::lock_guard lock(m_ChangedFlatRootsMutex);
            m_ChangedFlatRoots.insert(m_ChangedFlatRoots.end(), tl_ChangedRoots.begin(),
                                      tl_ChangedRoots.end());
        }
        total.fetch_add(chunkCount, std::memory_order_relaxed);
    });
    m_LastFlatCount = total.load(std::memory_order_relaxed);

    if (filterOn) {
        m_FlatGate.LastRunVersion = entryVersion;
        m_FlatGateValid = true;
    }
}

bool TransformHierarchySystem::HasParentColumnChange(ECS::World& world,
                                                     uint64 entryVersion) {
    using namespace GameEngine::ECS;

    bool changed = false;
    if (ChangeFilter::Enabled()) {
        if (!m_ParentGateValid)
            m_ParentGate.LastRunVersion = 0;
        auto q = world.Query<Read<Parent>>();
        q.IncludeDisabled();
        q.Changed<Parent>(m_ParentGate);
        q.BatchEach([&](const Parent*, std::size_t) { changed = true; });
    } else if (m_HasHierarchy) {
        // Diagnostic filter-off mode still preserves cached topology. Compare
        // the actual Parent handles instead of rebuilding every frame.
        auto q = world.Query<Read<Parent>>();
        q.IncludeDisabled();
        q.Each([&](EntityHandle entity, const Parent& parent) {
            const size_t index = FindNodeIndex(entity);
            if (index != SIZE_MAX && m_Nodes[index].parentEntity != parent.parent)
                changed = true;
        });
    } else {
        // A flat world only needs rebuilding if a real edge appears.
        changed = HasAnyValidParent(world);
    }

    m_ParentGate.LastRunVersion = entryVersion;
    m_ParentGateValid = true;
    return changed;
}

size_t TransformHierarchySystem::FindNodeIndex(ECS::EntityHandle entity) const {
    if (entity.index >= m_NodeIndexByEntityIndex.size())
        return SIZE_MAX;
    const uint32 nodeIndex = m_NodeIndexByEntityIndex[entity.index];
    if (nodeIndex != std::numeric_limits<uint32>::max() &&
        nodeIndex < m_Nodes.size() && m_Nodes[nodeIndex].entity == entity) {
        return static_cast<size_t>(nodeIndex);
    }
    return SIZE_MAX;
}

void TransformHierarchySystem::AddNode(ECS::EntityHandle entity, const Transform& local,
                                       WorldTransform& worldTransform, ECS::EntityHandle parent) {
    Node node{};
    node.entity = entity;
    node.local = &local;
    node.world = &worldTransform;
    node.worldRead = &worldTransform;
    node.parentEntity = parent;
    std::memcpy(node.localSnapshot.data(), local.matrix, sizeof(local.matrix));
    m_Nodes.push_back(node);
}

bool TransformHierarchySystem::HasDirtyAncestor(size_t index) {
    // Climb until a dirty node, a node already resolved this pass, or the
    // root; every clean node passed on the way gets the same answer.
    bool covered = false;
    m_CoverPath.clear();
    for (size_t up = m_ResolvedParent[index]; up != SIZE_MAX; up = m_ResolvedParent[up]) {
        if (m_DirtyPassByNode[up] == m_DirtyPass) {
            covered = true;
            break;
        }
        if (m_CoveredPassByNode[up] == m_DirtyPass) {
            covered = m_CoveredByNode[up] != 0u;
            break;
        }
        m_CoverPath.push_back(up);
    }
    for (size_t node : m_CoverPath) {
        m_CoveredPassByNode[node] = m_DirtyPass;
        m_CoveredByNode[node] = covered ? 1u : 0u;
    }
    return covered;
}

void TransformHierarchySystem::AddFlatOwnedRoots(ECS::World& world) {
    const size_t holders = m_Nodes.size();
    for (size_t index = 0; index < holders; ++index) {
        const ECS::EntityHandle parent = m_Nodes[index].parentEntity;
        if (!parent.IsValid() || FindNodeIndex(parent) != SIZE_MAX || !world.IsValid(parent))
            continue;
        // A parent with a Parent component of its own is a holder already, or
        // has no Transform; either way it is not a flat-owned root.
        if (world.GetComponent<Parent>(parent))
            continue;
        const Transform* local = world.GetComponent<Transform>(parent);
        const WorldTransform* worldTransform = world.GetComponent<WorldTransform>(parent);
        if (!local || !worldTransform)
            continue; // the child is re-rooted with a warning when the CSR resolves
        Node node{};
        node.entity = parent;
        node.local = local;
        node.worldRead = worldTransform;
        node.flatOwned = true;
        std::memcpy(node.localSnapshot.data(), local->matrix, sizeof(local->matrix));
        if (parent.index >= m_NodeIndexByEntityIndex.size())
            m_NodeIndexByEntityIndex.resize(static_cast<size_t>(parent.index) + 1,
                                            std::numeric_limits<uint32>::max());
        m_NodeIndexByEntityIndex[parent.index] = static_cast<uint32>(m_Nodes.size());
        m_Nodes.push_back(node);
    }
}

size_t TransformHierarchySystem::PropagateSubtree(size_t rootIndex, std::vector<size_t>& stack,
                                                  std::vector<ECS::EntityHandle>& changed) {
    if (rootIndex >= m_Nodes.size())
        return 0;

    size_t visited = 0;
    stack.clear();
    stack.push_back(rootIndex);
    while (!stack.empty()) {
        const size_t index = stack.back();
        stack.pop_back();

        Node& node = m_Nodes[index];
        const size_t parentIndex = m_ResolvedParent[index];
        if (parentIndex != SIZE_MAX) {
            float32 next[16];
            MultiplyColumnMajor4x4(m_Nodes[parentIndex].worldRead->matrix, node.local->matrix, next);
            if (std::memcmp(node.world->matrix, next, sizeof(next)) != 0) {
                std::memcpy(node.world->matrix, next, sizeof(next));
                ++node.world->Version;
                changed.push_back(node.entity);
            }
        } else if (!node.flatOwned && WriteRootWorldTransform(node.local->matrix, *node.world)) {
            changed.push_back(node.entity);
        }
        ++visited;

        // Reverse insertion preserves the cached child order during the LIFO
        // walk while avoiding recursion depth limits on very deep scenes.
        for (size_t cursor = m_ChildOffsets[index + 1];
             cursor > m_ChildOffsets[index]; --cursor) {
            stack.push_back(m_ChildIndices[cursor - 1]);
        }
    }
    return visited;
}

void TransformHierarchySystem::PropagateRootChunk(std::span<const size_t> roots, size_t begin, size_t end,
                                                  size_t chunk) {
    m_ChunkChanged[chunk].clear();
    size_t visited = 0;
    for (size_t r = begin; r < end; ++r)
        visited += PropagateSubtree(roots[r], m_ChunkStacks[chunk], m_ChunkChanged[chunk]);
    m_ChunkVisited[chunk] = visited;
}

void TransformHierarchySystem::PropagateRoots(ECS::World& world, std::span<const size_t> roots,
                                              StampCollection stamps) {
    const auto worldTransformType = ECS::GetComponentTypeId<WorldTransform>();
    JobSystem::WorkStealingThreadPool* js = m_ForceSerial ? nullptr : world.GetJobSystem();
    size_t visited = 0;
    m_StampScratch.clear();
    if (!js || roots.size() < kParallelMinRoots) {
        m_ChunkStacks.resize(std::max<size_t>(m_ChunkStacks.size(), 1));
        for (size_t root : roots)
            visited += PropagateSubtree(root, m_ChunkStacks[0], m_StampScratch);
    } else {
        // The roots' subtrees are disjoint, so each chunk writes only its own
        // nodes; the shared CSR and the parents' world matrices are read-only.
        const size_t workers = std::max<size_t>(1, js->GetWorkerCount());
        const size_t chunkSize =
            std::max(kMinRootsPerChunk, (roots.size() + workers * 4 - 1) / (workers * 4));
        const size_t chunkCount = (roots.size() + chunkSize - 1) / chunkSize;
        if (m_ChunkStacks.size() < chunkCount) {
            m_ChunkStacks.resize(chunkCount);
            m_ChunkChanged.resize(chunkCount);
            m_ChunkVisited.resize(chunkCount);
        }
        JobSystem::JobCounter counter;
        for (size_t c = 0; c < chunkCount; ++c) {
            const size_t begin = c * chunkSize;
            const size_t end = std::min(roots.size(), begin + chunkSize);
            js->Run([this, roots, begin, end, c] { PropagateRootChunk(roots, begin, end, c); }, counter);
        }
        js->Wait(counter);
        for (size_t c = 0; c < chunkCount; ++c) {
            visited += m_ChunkVisited[c];
            m_StampScratch.insert(m_StampScratch.end(), m_ChunkChanged[c].begin(),
                                  m_ChunkChanged[c].end());
        }
    }
    m_LastPropagatedNodeCount = visited;

    if (m_StampScratch.empty())
        return;
    world.EmitComponentDirtyBatch(worldTransformType, m_StampScratch.data(), m_StampScratch.size());
    // Cached-node writes go through pointers captured at rebuild, outside any
    // query visit, so nothing else stamps the WorldTransform grant that
    // Changed<WorldTransform> consumers filter on. The rebuild pass is the
    // exception: its gather bound Write<WorldTransform> and stamped already.
    if (stamps == StampCollection::Collect)
        world.StampComponentWriteBatch(m_StampScratch.data(), m_StampScratch.size(), worldTransformType);
    m_StampScratch.clear();
}

void TransformHierarchySystem::RebuildTopology(ECS::World& world) {
    using namespace GameEngine::ECS;

    ++m_TopologyBuildCount;
    m_LastPropagatedNodeCount = 0;
    m_HasHierarchy = m_ForceSerial || HasAnyValidParent(world);
    m_FlatGateValid = false;

    m_Nodes.clear();
    m_NodeIndexByEntityIndex.clear();
    if (!m_HasHierarchy) {
        m_ResolvedParent.clear();
        m_ChildOffsets.clear();
        m_ChildIndices.clear();
        m_Roots.clear();
        return;
    }

    // The cached nodes: every entity under the test seam, otherwise only the
    // Parent-holders (their Write<WorldTransform> visit grants the rebuild's
    // own writes) plus the roots above them, which the flat pass owns.
    if (m_ForceSerial) {
        auto q = world.Query<Read<Transform>, Write<WorldTransform>, Optional<Parent>>();
        q.IncludeDisabled();
        q.Each([this](EntityHandle entity, const Transform& local, WorldTransform& worldTransform,
                      const Parent* parent) {
            AddNode(entity, local, worldTransform, parent ? parent->parent : EntityHandle{});
        });
    } else {
        auto q = world.Query<Read<Transform>, Write<WorldTransform>, Read<Parent>>();
        q.IncludeDisabled();
        q.Each([this](EntityHandle entity, const Transform& local, WorldTransform& worldTransform,
                      const Parent& parent) { AddNode(entity, local, worldTransform, parent.parent); });
    }

    size_t maxEntityIndex = 0;
    for (const Node& node : m_Nodes)
        maxEntityIndex = std::max(maxEntityIndex, static_cast<size_t>(node.entity.index));
    m_NodeIndexByEntityIndex.assign(
        m_Nodes.empty() ? 0 : maxEntityIndex + 1,
        std::numeric_limits<uint32>::max());
    for (size_t index = 0; index < m_Nodes.size(); ++index) {
        m_NodeIndexByEntityIndex[m_Nodes[index].entity.index] =
            static_cast<uint32>(index);
    }
    if (!m_ForceSerial)
        AddFlatOwnedRoots(world);
    const size_t nodeCount = m_Nodes.size();

    m_ResolvedParent.assign(nodeCount, SIZE_MAX);
    for (size_t index = 0; index < nodeCount; ++index) {
        const EntityHandle parent = m_Nodes[index].parentEntity;
        if (!parent.IsValid())
            continue;
        const size_t parentIndex = FindNodeIndex(parent);
        if (parentIndex != SIZE_MAX) {
            m_ResolvedParent[index] = parentIndex;
        } else {
            Logger::Log::Warning(
                "TransformHierarchySystem: parent entity {} for child {} missing Transform; treating child as root",
                parent.id, m_Nodes[index].entity.id);
        }
    }

    // Break one edge in every cycle so the cached graph is a forest. This is
    // performed only during topology rebuild and makes all later dirty walks
    // iterative, bounded, and deterministic.
    m_TopologyState.assign(nodeCount, 0u);
    m_TraversalStack.clear();
    for (size_t start = 0; start < nodeCount; ++start) {
        if (m_TopologyState[start] != 0u)
            continue;
        size_t cursor = start;
        m_TraversalStack.clear();
        while (cursor != SIZE_MAX && m_TopologyState[cursor] == 0u) {
            m_TopologyState[cursor] = 1u;
            m_TraversalStack.push_back(cursor);
            cursor = m_ResolvedParent[cursor];
        }
        if (cursor != SIZE_MAX && m_TopologyState[cursor] == 1u) {
            Logger::Log::Error(
                "TransformHierarchySystem: cycle detected involving entity {}; treating it as a root",
                m_Nodes[cursor].entity.id);
            m_ResolvedParent[cursor] = SIZE_MAX;
        }
        for (size_t index : m_TraversalStack)
            m_TopologyState[index] = 2u;
    }

    m_ChildOffsets.assign(nodeCount + 1, 0);
    for (size_t index = 0; index < nodeCount; ++index) {
        const size_t parentIndex = m_ResolvedParent[index];
        if (parentIndex != SIZE_MAX)
            ++m_ChildOffsets[parentIndex + 1];
    }
    for (size_t index = 0; index < nodeCount; ++index)
        m_ChildOffsets[index + 1] += m_ChildOffsets[index];

    m_ChildIndices.resize(m_ChildOffsets[nodeCount]);
    m_TraversalStack.assign(m_ChildOffsets.begin(), m_ChildOffsets.end() - 1);
    for (size_t index = 0; index < nodeCount; ++index) {
        const size_t parentIndex = m_ResolvedParent[index];
        if (parentIndex != SIZE_MAX)
            m_ChildIndices[m_TraversalStack[parentIndex]++] = index;
    }

    m_Roots.clear();
    for (size_t index = 0; index < nodeCount; ++index) {
        if (m_ResolvedParent[index] == SIZE_MAX)
            m_Roots.push_back(index);
    }

    m_DirtyPassByNode.assign(nodeCount, 0u);
    m_CoveredPassByNode.assign(nodeCount, 0u);
    m_CoveredByNode.assign(nodeCount, 0u);
    m_DirtyPass = 0;
}

void TransformHierarchySystem::ResolveChangedSubtrees(ECS::World& world,
                                                       double ensureMs) {
    using namespace GameEngine::ECS;

    const auto tDetectStart = Mark();
    const uint64 entryVersion = world.GetGlobalSystemVersion();
    auto q = world.Query<Read<Transform>>();
    q.IncludeDisabled();
    if (!m_ForceSerial)
        q.With<Parent>(); // entities without one resolved on the flat pass
    if (ChangeFilter::Enabled()) {
        if (!m_TransformGateValid)
            m_TransformGate.LastRunVersion = 0;
        q.Changed<Transform>(m_TransformGate);
    }

    m_DirtyNodes.clear();
    q.Each([&](EntityHandle entity, const Transform& local) {
        const size_t index = FindNodeIndex(entity);
        if (index == SIZE_MAX)
            return;
        Node& node = m_Nodes[index];
        if (std::memcmp(node.localSnapshot.data(), local.matrix,
                        sizeof(local.matrix)) == 0)
            return;
        std::memcpy(node.localSnapshot.data(), local.matrix,
                    sizeof(local.matrix));
        m_DirtyNodes.push_back(index);
    });
    m_TransformGate.LastRunVersion = entryVersion;
    m_TransformGateValid = true;
    m_DirtyNodes.insert(m_DirtyNodes.end(), m_ChangedFlatRoots.begin(), m_ChangedFlatRoots.end());
    const auto tFilterStart = Mark();

    m_LastPropagatedNodeCount = 0;
    if (m_DirtyNodes.empty())
        return;

    // Keep only the topmost dirty nodes: a dirty node under a dirty ancestor
    // is covered by the ancestor's walk. What is left are disjoint subtrees,
    // which is what lets them propagate concurrently.
    if (++m_DirtyPass == 0) {
        std::fill(m_DirtyPassByNode.begin(), m_DirtyPassByNode.end(), 0u);
        std::fill(m_CoveredPassByNode.begin(), m_CoveredPassByNode.end(), 0u);
        m_DirtyPass = 1;
    }
    for (size_t index : m_DirtyNodes)
        m_DirtyPassByNode[index] = m_DirtyPass;
    std::erase_if(m_DirtyNodes, [this](size_t index) { return HasDirtyAncestor(index); });
    const size_t topmostCount = m_DirtyNodes.size();
    const auto tPropagateStart = Mark();

    PropagateRoots(world, m_DirtyNodes, StampCollection::Collect);

    if (kProfileEnabled) {
        const auto tEnd = Mark();
        g_hierarchyProfiler.Accumulate(
            ensureMs,
            DeltaMs(tDetectStart, tFilterStart),
            DeltaMs(tFilterStart, tPropagateStart),
            0.0,
            DeltaMs(tPropagateStart, tEnd),
            m_Nodes.size(), topmostCount,
            m_LastPropagatedNodeCount);
    }
}

} } // namespace GameEngine::Engine::Renderer
