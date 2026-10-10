#pragma once

// HLOD scene reconciliation (design v0.2 §3.3 / F6). Bridges a persisted
// .gehlod (content-keyed: BakedCluster members carry stable GUIDs, not runtime
// slot indices) to the live scene (entity-keyed). Given a per-entity StableId
// index (built by the caller from the live SceneEntityTags, the same derivation
// the member-gather used) it resolves each baked cluster's members to live
// EntityHandles and reports which clusters are missing members or have their
// per-cluster source hash disagree with the live scene (stale). Pure — no ECS,
// no GPU — so the reconcile policy is unit-tested directly.

#include "Assets/HlodCache.h"
#include "AssetCore/GUID.h"
#include "ECS/ECS.h" // EntityHandle
#include "Types/Types.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace GameEngine {
namespace Hlod {

// One reconciled cluster: the live member entities (resolved from the baked
// StableIds), plus the switch inputs copied from the baked cluster. Members that
// could not be resolved to a live entity are dropped from Members and counted in
// MissingMembers; a cluster with any missing member is marked Stale (its proxy
// no longer represents the live scene, so it must fall back to members-only,
// §3.4).
struct ReconciledCluster {
    uint32 ClusterId = 0xFFFFFFFFu; // index into the baked scene's Clusters
    float  SphereCenter[3] = {0.0f, 0.0f, 0.0f};
    float  SphereRadius = 0.0f;
    std::vector<GameEngine::ECS::EntityHandle> Members;
    uint32 MissingMembers = 0;
    bool   Stale = false; // missing members or a live-source-hash mismatch
};

struct ReconcileResult {
    std::vector<ReconciledCluster> Clusters;
    uint32 TotalMissingMembers = 0;
    uint32 StaleClusters = 0;
};

// The live-scene StableId -> EntityHandle map plus each entity's currently
// resolved cluster source-hash inputs (material GUID, content hash, mesh handle
// key, chosen LOD, transform) so a per-cluster staleness check can recompute the
// cluster source hash from the LIVE scene and compare it against the baked one
// (C3: a material/mesh reassignment or a transform edit perturbs the hash).
struct LiveMemberState {
    GameEngine::ECS::EntityHandle Entity;
    BakedMemberRef Resolved; // the C3 key inputs as they are in the live scene now
};

using LiveMemberIndex = std::unordered_map<GUID, LiveMemberState>;

// Reconcile a baked scene against the live member index. For each baked cluster,
// resolve its members by StableId; a cluster is stale if any member is missing
// or the live-recomputed cluster source hash differs from the baked one.
ReconcileResult ReconcileBakedScene(const HlodBakedScene& baked, const LiveMemberIndex& live);

} // namespace Hlod
} // namespace GameEngine
