#pragma once

#include "Types/Types.h"

#include <type_traits>

namespace GameEngine {
namespace Components {

// Authored opt-in that makes a scene's eligible static meshes HLOD candidates.
// Presence of one HLODVolume is the scene-level opt-in: with none present the
// HLOD subsystem does no per-frame work and the scene pays zero overhead (design
// v0.2 §3.1, §9 slice 5). The component carries the offline bake configuration —
// grid cell size, the benefit gate, and the resident-proxy-VB budget — not any
// runtime state; a baked cluster set lives in the sidecar .gehlod and the
// HlodRuntime cluster table (proxies carry the HLODProxy tag), never here.
//
// v1 scope: the volume applies to every eligible static in the world (non-
// skinned, triangle-topology, default per-instance custom0). Spatial scoping of
// a volume to a sub-region is a follow-up; the benefit heuristic + far-field
// switch already exclude the near/dense cells that would regress.
struct HLODVolume
{
    // Grid cell size in world units. Eligible members are assigned single-owner
    // by world-AABB center to floor((center - origin) / CellSize). A member
    // whose world AABB exceeds one cell on any axis is excluded and drawn
    // directly (logged at bake). 48-64 m is the design's candidate range.
    float32 CellSize = 64.0f;

    // Benefit gate: a cluster whose instancing ratio r = members / distinct
    // meshes exceeds this is NOT baked — merging de-instances, so a high-r cell
    // would trade N cheap instance records for an r-times-larger unique vertex
    // buffer (design §2.1/§2.2). Low-r far clusters are where HLOD pays off.
    float32 MaxInstancingRatio = 4.0f;

    // A cluster with fewer eligible members than this is skipped: too little
    // record/caster saving to justify a de-instanced proxy VB.
    uint32 MinMembers = 8u;

    // Per-project resident-proxy-VB cap (megabytes). Admitted clusters are
    // ranked by benefit/ΔVB and taken greedily until this budget is spent;
    // overflow clusters stay members-only. A hard cap so de-instancing can
    // never silently blow resident VRAM (design §2.2 + R1).
    uint32 VBBudgetMB = 64u;

    // Runtime switch threshold (design §5.2). A cluster becomes proxy-active when
    // its screen coverage (clusterRadius * projScaleY / distance, the ge_SelectLOD
    // metric) falls below this — i.e. when it is far enough that the members are
    // already at their coarsest LOD. Lower = proxies engage farther away. The
    // proxy is effectively "LOD4+".
    float32 SwitchCoverage = 0.08f;

    // Hysteresis band as a fraction of SwitchCoverage. The cluster stays
    // proxy-active until coverage rises past SwitchCoverage * (1 + this), so a
    // camera hovering on the boundary does not thrash the residency flip.
    float32 SwitchHysteresis = 0.25f;
};

static_assert(std::is_trivially_copyable_v<HLODVolume>,
              "HLODVolume must be trivially copyable for ECS");
static_assert(std::is_standard_layout_v<HLODVolume>,
              "HLODVolume must be standard layout for ECS");

} // namespace Components
} // namespace GameEngine
