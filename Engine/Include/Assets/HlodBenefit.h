#pragma once

// HLOD per-cluster benefit heuristic + resident-proxy-VB budget admission
// (design v0.2 §2.2). Merging is anti-instancing: it trades N cheap 32 B
// instance records for one big unique vertex buffer whose size scales with the
// cluster's instancing ratio r = members / distinct meshes. So a cluster is only
// proxied when it is LOW-instancing (r ≤ MaxInstancingRatio) and has enough
// members to save records/casters; survivors are then ranked by benefit/ΔVB and
// admitted greedily until the per-project VB budget is spent. Pure and testable.

#include "Assets/HlodClustering.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>
#include <vector>

namespace GameEngine {
namespace Hlod {

// Resident VB byte estimate per member vertex: the core interleaved stride
// (position/normal/uv/tangent). Optional streams (color, uv1) add to the baked
// proxy on top of this; the estimate is the dominant term and drives admission.
inline constexpr uint32 kHlodCoreVertexStrideBytes = 48u;

// Why a cluster is (not) proxied. Recorded per cluster so the bake can log
// skipped-with-reason and the debug overlay can tint excluded cells.
enum class ClusterDisposition : uint8 {
    Admitted,        // proxied: cleared the gate and fits the VB budget
    TooFewMembers,   // members < MinMembers — too little record/caster saving
    HighInstancing,  // r > MaxInstancingRatio — de-instancing would regress VB
    OverBudget,      // gate passed but greedy VB-budget admission ran out
};

// Per-cluster benefit accounting, parallel to ClusterTable::Clusters.
struct ClusterBenefit {
    ClusterDisposition Disposition = ClusterDisposition::TooFewMembers;
    uint32 MemberCount = 0;        // N
    uint32 DistinctMeshes = 0;     // D_eff (distinct member mesh content hashes)
    uint32 SubmeshCount = 0;       // M (distinct member material GUIDs = proxy submeshes)
    uint32 CastingMembers = 0;     // members with CastsShadow
    float  InstancingRatio = 0.0f; // r = N / D_eff
    int64  Benefit = 0;            // recordsSaved (N−M) + castersSaved (casting−M)
    uint64 EstimatedVbBytes = 0;   // ΔVB estimate (Σ member vertices × core stride)
};

// Evaluate every cluster and mark its disposition. Gate order: too-few → high-
// instancing → benefit rank + VB budget. `outBenefits` is resized to match
// `table.Clusters`. Deterministic for fixed (table, members, config): ties in
// benefit/ΔVB break by ascending cluster index.
void EvaluateClusterBenefit(const ClusterTable& table,
                            std::span<const MemberInput> members,
                            const GridConfig& config,
                            std::vector<ClusterBenefit>& outBenefits);

} // namespace Hlod
} // namespace GameEngine
