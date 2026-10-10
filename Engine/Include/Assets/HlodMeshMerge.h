#pragma once

// HLOD proxy merge (design v0.2 §4.1/§4.2). Combines a cluster's eligible static
// members into one proxy mesh per distinct RESOLVED MATERIAL GUID (submesh-per-
// material), baking each member's coarsest-LOD geometry through its cluster-
// relative world transform. Models the transform-bake math of
// MergeStaticPrimitivesByMaterial (ModelAssetLoadGltf.cpp) — positions by M·p, normals
// by the inverse-transpose, tangent by R with a handedness + winding flip on a
// negative determinant — but grouped by material GUID (members come from
// different model assets, so a within-model MaterialIndex is not comparable),
// always baking (an HLOD member always needs its world transform folded in), and
// with no reduction gate (the benefit heuristic decides admission separately).
//
// v1 preserves the core interleaved stream plus optional vertex color (COLOR0)
// and a second UV set (TEXCOORD1). Extra UV sets (UV2..UV7) and skinning are out
// of scope — eligibility already excludes skinned members.

#include "AssetCore/GUID.h"
#include "Types/Types.h"

#include <cstdint>
#include <span>

namespace GameEngine {

struct Mesh;

namespace Hlod {

// One member's geometry + bake inputs. `Source` must outlive the merge call.
struct MergeMember {
    const Mesh* Source = nullptr;   // member submesh (LOD0 vertices + ExtraLODs)
    GUID   MaterialGuid;            // resolved runtime material identity (group key)
    uint32 ChosenLod = 0u;         // which LOD's index buffer to bake (coarsest)
    float  Transform[16] = {       // cluster-relative world matrix (column-major)
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f};
};

// The baked proxy: one Mesh per distinct material GUID (in ascending GUID order
// for determinism), the parallel material GUID list, and the total resident core
// VB bytes (Σ baked vertices × sizeof(Vertex)).
struct MergedProxy {
    Vector<Mesh> Submeshes;
    Vector<GUID> SubmeshMaterials;
    uint64 CoreVertexBytes = 0;
};

// Merge a cluster's members. Each submesh's MaterialIndex is set to its submesh
// ordinal; the true material identity is SubmeshMaterials[ordinal].
MergedProxy MergeClusterMembers(std::span<const MergeMember> members);

} // namespace Hlod
} // namespace GameEngine
