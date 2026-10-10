#pragma once

// HLOD offline clustering (design v0.2 §3.1). Pure, engine-agnostic: it consumes
// a set of eligible static members (identity + world-space spatial data already
// resolved from the ECS scene) and a grid config, and partitions them
// single-owner-by-AABB-center into world-space grid cells. No ECS, no GPU, no
// I/O — so it is exercised entirely by unit tests. Membership is deterministic
// for a given member set + config (cell-coord order, then ascending member
// index) so a re-bake of an unchanged scene reproduces the same cluster table.

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine {
namespace Hlod {

// Per-volume grid + admission configuration, resolved from an HLODVolume
// component (Components/Rendering/HLODVolume.h). Kept as a plain value so the
// clustering + bake pipeline never depends on the ECS component type.
struct GridConfig {
    float    CellSize = 64.0f;          // world units per cell edge
    float    GridOrigin[3] = {0.0f, 0.0f, 0.0f}; // world anchor of the cell lattice
    float    MaxInstancingRatio = 4.0f; // benefit gate: exclude cells with r above this
    uint32   MinMembers = 8u;           // clusters below this are not proxied
    uint64   VbBudgetBytes = 64ull * 1024ull * 1024ull; // resident-proxy-VB cap
};

// One eligible member's clustering + bake-key inputs. Identity is a STABLE GUID
// (entity/asset), never a runtime slot index (design §3.3 / F6). The C3 bake-key
// fields (material GUID, mesh handle key, content hash, chosen LOD) travel with
// the member so a later material/mesh reassignment perturbs the cluster key.
struct MemberInput {
    GUID     StableId;                // entity/asset stable GUID
    uint64   MeshContentHash = 0;     // member source geometry content hash (C3)
    GUID     MaterialGuid;            // resolved material GUID (C3)
    uint64   MeshHandleKey = 0;       // mesh handle / submesh selector fold (C3)
    float    WorldMatrix[16] = {      // column-major world transform (bake input)
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
    float    WorldAabbMin[3] = {0.0f, 0.0f, 0.0f}; // world-space bounds
    float    WorldAabbMax[3] = {0.0f, 0.0f, 0.0f};
    uint32   VertexCount = 0u;        // baked verts: chosen level's own block, else LOD0 (VB budget)
    uint32   ChosenLod = 0u;          // coarsest LOD index baked into the proxy
    bool     CastsShadow = true;      // for the caster-saving benefit term
};

// Integer cell coordinate. floor((center - origin) / cellSize) per axis.
struct CellCoord {
    int32 X = 0;
    int32 Y = 0;
    int32 Z = 0;

    bool operator==(const CellCoord& o) const { return X == o.X && Y == o.Y && Z == o.Z; }
    // Lexicographic ordering for deterministic cluster emission.
    bool operator<(const CellCoord& o) const {
        if (X != o.X) return X < o.X;
        if (Y != o.Y) return Y < o.Y;
        return Z < o.Z;
    }
};

// One grid cell with at least one owned member. Spatial fields are the union of
// member world AABBs; the bounding sphere (center + radius) feeds the runtime
// screen-coverage switch. Admission fields are filled by the benefit pass
// (HlodBenefit.h); BuildClusters leaves them at their defaults.
struct Cluster {
    CellCoord Cell;
    float     BoundsMin[3] = {0.0f, 0.0f, 0.0f};
    float     BoundsMax[3] = {0.0f, 0.0f, 0.0f};
    float     SphereCenter[3] = {0.0f, 0.0f, 0.0f}; // proxy WorldTransform origin
    float     SphereRadius = 0.0f;
    std::vector<uint32> MemberIndices; // indices into the BuildClusters member span, ascending
};

// Output of clustering: the owned clusters (in ascending cell-coord order) plus
// the members that were excluded because their world AABB exceeds one cell on
// some axis (drawn directly; the caller logs a warning per the design).
struct ClusterTable {
    std::vector<Cluster> Clusters;
    std::vector<uint32>  OversizedMembers; // indices into the member span
};

// Partition `members` into grid cells. Single-owner by world-AABB center;
// members larger than one cell are moved to OversizedMembers. Deterministic for
// a fixed (members, config). A zero/negative CellSize yields an empty table.
ClusterTable BuildClusters(std::span<const MemberInput> members, const GridConfig& config);

} // namespace Hlod
} // namespace GameEngine
