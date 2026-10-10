#pragma once

#include <vector>

#include "Mathematics/Geometry.h"
#include "Mathematics/Types.h"
#include "MeshPicking/PickTypes.h"
#include "Types/Types.h"

namespace GameEngine::MeshPicking
{

// Triangle count above which automatic-mode raycasts switch from brute force
// to BVH traversal. Tuned in SplineTool's original implementation; below this
// the build/traversal overhead exceeds the savings.
constexpr uint32 kMeshAutoBvhThreshold = 256u;

// Möller-Trumbore ray/triangle intersection. Returns the parametric t along
// the ray (positive forward) on hit, plus the barycentric (u, v) of the hit
// point. Backface-culling-free: hits from either side are reported (callers
// gate front-facing in higher-level options).
bool RayTriangle(const Mathematics::Vector3& origin,
                 const Mathematics::Vector3& direction,
                 const Mathematics::Vector3& v0,
                 const Mathematics::Vector3& v1,
                 const Mathematics::Vector3& v2,
                 float32& outT,
                 float32& outU,
                 float32& outV);

// Brute-force ray vs every triangle in the mesh. Returns the closest hit.
// Use for small meshes (< kMeshAutoBvhThreshold) where the BVH build cost
// would dominate the savings.
bool RayMesh(const MeshView& mesh,
             const Mathematics::Vector3& origin,
             const Mathematics::Vector3& direction,
             PickHit& outHit);

// Static-mesh bounding volume hierarchy. Built once per (asset, submesh)
// and reused across many picks.
//
// Phase A: midpoint split. Phase C: binned SAH builder. Phase D (this):
// collapse the binary SAH tree into a CWBVH-style 8-wide BVH with each
// child AABB stored as 8-bit per-axis quantization relative to the
// parent's full-precision AABB. AVX2 ray-vs-8-AABB SLAB test traverses
// 8 children in one vector op. Memory drops ~3-5x vs uncompressed
// BVH8 (8-bit per axis vs float32 per axis), L1 hit rate goes way up,
// tree depth shrinks to ~log_8 N (a third of binary's log_2 N).
//
// Public API stays identical across all phases.
class MeshBvh
{
public:
    // Build a BVH over the given triangle mesh. Returns an empty BVH if the
    // input is invalid or has zero triangles. Vertex data is copied into the
    // BVH (it owns its triangles) so the source MeshView need not outlive it.
    static MeshBvh Build(const MeshView& mesh);

    // Cast a ray (in mesh-local space; caller transforms world->local) and
    // return the closest hit. Returns false on miss.
    bool Raycast(const Mathematics::Vector3& origin,
                 const Mathematics::Vector3& direction,
                 PickHit& outHit) const;

    bool IsEmpty() const { return m_Nodes8.empty(); }
    uint32 TriangleCount() const { return static_cast<uint32>(m_Triangles.size()); }
    Mathematics::AABB RootBounds() const { return m_RootBounds; }

    // Maximum triangles allowed in a leaf node before splitting (binary
    // intermediate during Build; preserved in BVH8 leaves).
    static constexpr uint32 kLeafSize = 8u;

    // Serialize the BVH to a byte buffer in a stable layout. Triangle vertex
    // positions are NOT written — only the source-triangle index per BVH
    // triangle, plus tri-indices and CWBVH8 nodes. The deserializer rebuilds
    // vertex positions from the source MeshView.
    //
    // Stable across runs on x86_64; not portable across endianness or pointer
    // width. Callers wrap with their own file header (magic, version,
    // fingerprint, etc.).
    void Serialize(std::vector<uint8>& outBytes) const;

    // Inverse of Serialize. The source MeshView must match the mesh the BVH
    // was originally built from (same indices and vertex layout). Returns
    // false on a malformed buffer or when the source mesh is incompatible.
    bool Deserialize(const std::vector<uint8>& bytes, const MeshView& source);

    // Serialized-format magic + version. Bump invalidates cached files.
    static constexpr uint32 kSerializedMagic   = 0x424E5742u; // 'BVNB' (le)
    static constexpr uint32 kSerializedVersion = 1u;

private:
    struct Triangle
    {
        Mathematics::Vector3 V0;
        Mathematics::Vector3 V1;
        Mathematics::Vector3 V2;
        uint32               OriginalIndex = 0u;
    };

    // Binary BVH node. Live only during Build as the binned-SAH
    // intermediate; collapsed into BvhNode8 storage and discarded.
    struct BinaryNode
    {
        Mathematics::Vector3 BoundsMin;
        Mathematics::Vector3 BoundsMax;
        uint32               Left     = 0u;
        uint32               Right    = 0u;
        uint32               FirstTri = 0u;
        uint32               TriCount = 0u;   // 0 = internal
    };

    // CWBVH 8-wide node. Each child AABB is 8-bit per axis (low + high)
    // quantized relative to the parent's full-precision AABB:
    //   childMin.axis = ParentMin.axis + (ParentMax.axis - ParentMin.axis) * (qMin / 255)
    //   childMax.axis = ParentMin.axis + (ParentMax.axis - ParentMin.axis) * (qMax / 255)
    // Quantization is conservative: qMin floors, qMax ceils, so the
    // dequantized AABB always encloses the true child AABB. Rays that
    // would hit may produce false-positives at child boundaries (slightly
    // wider AABB), but never false-negatives (no missed hits).
    //
    // Per child slot:
    //   - if ChildTriCount[i] > 0: leaf; ChildIdx[i] = first index into
    //     m_TriIndices, ChildTriCount[i] = triangle count.
    //   - if ChildTriCount[i] == 0 AND ValidMask bit i set: internal;
    //     ChildIdx[i] = next BvhNode8 index in m_Nodes8.
    //   - if ValidMask bit i clear: slot is empty (collapsed fewer than 8).
    struct alignas(32) BvhNode8
    {
        // Parent AABB at full precision (24 B). Children are quantized
        // against this range.
        Mathematics::Vector3 ParentMin;
        Mathematics::Vector3 ParentMax;

        // SoA quantized child bounds, one byte per child per axis. AVX2
        // can load 8 uint8s as __m128i then widen to __m256 floats with
        // _mm256_cvtepu8_epi32 + _mm256_cvtepi32_ps.
        uint8 ChildMinX[8];   // 8 B
        uint8 ChildMinY[8];
        uint8 ChildMinZ[8];
        uint8 ChildMaxX[8];
        uint8 ChildMaxY[8];
        uint8 ChildMaxZ[8];

        // Cold: per-child node/leaf metadata. (32 + 16 = 48 B)
        uint32 ChildIdx[8];        // child node OR first triangle
        uint16 ChildTriCount[8];   // 0 = internal node, >0 = leaf

        uint8  ValidMask;          // bit i set = slot i populated
        uint8  _pad[7];            // align next node to 32 B
    };
    static_assert(sizeof(BvhNode8) == 128,
                  "BvhNode8 size unexpected; AVX2 traversal assumes 128 B (2 cache lines)");

    uint32 BuildBinary(std::vector<BinaryNode>& bins, uint32 first, uint32 count);
    uint32 CollapseFromBinary(const std::vector<BinaryNode>& bins, uint32 binIdx);

    std::vector<Triangle> m_Triangles;
    // Build-time scratch for the shared binned-SAH kernel: vertex-mean
    // centroids (3 floats/tri) and triangle AABBs (6 floats/tri), computed
    // once in Build() and released after the binary stage.
    std::vector<float32> m_BuildCentroids;
    std::vector<float32> m_BuildTriBounds;
    std::vector<uint32>   m_TriIndices;
    std::vector<BvhNode8> m_Nodes8;
    Mathematics::AABB     m_RootBounds{};
};

}
