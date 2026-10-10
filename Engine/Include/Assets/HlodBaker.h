#pragma once

// HLOD offline bake driver (design v0.2 §4). Ties the pure stages together:
// clustering (single-owner grid) → benefit heuristic + VB-budget admission →
// per-admitted-cluster merge (submesh-per-material, cluster-relative bake) →
// a persistable HlodBakedScene with the full C3 bake key. No ECS/GPU/IO: the
// caller gathers members from the scene and, after this returns, registers the
// proxy meshes + spawns proxy entities + writes the .gehlod (HlodCache.h). Pure
// so the whole offline pipeline is unit-tested end to end.

#include "Assets/HlodBenefit.h"
#include "Assets/HlodCache.h"
#include "Assets/HlodClustering.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>

namespace GameEngine {

struct Mesh;

namespace Hlod {

// Aggregate bake outcome for logging / the acceptance report.
struct BakeStats {
    uint32 ClusterCount = 0;          // grid cells with ≥1 member
    uint32 AdmittedCount = 0;         // clusters baked into proxies
    uint32 SkippedTooFew = 0;         // < MinMembers
    uint32 SkippedHighInstancing = 0; // r > MaxInstancingRatio (anti-instancing gate)
    uint32 SkippedOverBudget = 0;     // dropped by the VB budget
    uint32 SkippedEmptyMerge = 0;     // admitted, but the merge produced no proxy geometry
    uint32 OversizedMembers = 0;      // AABB > cell, drawn directly
    uint64 ProxyVertexBytes = 0;      // Σ admitted proxy core VB (resident cost)
};

// Bake admitted clusters. `memberGeometry` is parallel to `members`:
// memberGeometry[i] is members[i]'s source submesh (LOD0 vertices + ExtraLODs);
// a null entry contributes no proxy triangles but still counts toward
// clustering + benefit. Returns the scene with Key/CellSize/GridOrigin filled;
// only Admitted clusters appear in Clusters. `outStats` may be null.
HlodBakedScene BakeScene(std::span<const MemberInput> members,
                         std::span<const Mesh* const> memberGeometry,
                         const GridConfig& config,
                         BakeStats* outStats = nullptr);

} // namespace Hlod
} // namespace GameEngine
