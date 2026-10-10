#pragma once

#include <span>
#include <vector>

#include "SceneBvh/BinnedSahSplit.h"
#include "SceneBvh/MeshGeometryView.h"
#include "SceneBvh/ThreadedBvh.h"
#include "Types/Types.h"

namespace JobSystem
{
class WorkStealingThreadPool;
}

namespace GameEngine::SceneBvh
{

// Leaf size cap. Matches the reference build (three-mesh-bvh maxLeafSize: 8).
// Must stay <= kThreadedBvhMaxLeafTriangles, the ceiling the leaf word's 8-bit
// count field imposes.
inline constexpr uint32 kMaxLeafTriangles = 8u;

// Cost of descending one interior node, expressed in ray/triangle tests. The
// SAH compares (kNodeTraversalCost + area-weighted child costs) against the
// cost of making a leaf here, which is simply the triangle count.
inline constexpr float32 kNodeTraversalCost = 1.0f;

// Leaves come out roughly half full, so a tree over N triangles lands near
// 2 * N / kExpectedLeafOccupancy nodes. Used only to size the initial node
// reservation; the array still grows if a mesh partitions worse than this.
inline constexpr uint32 kExpectedLeafOccupancy = kMaxLeafTriangles / 2u;

// Meshes per parallel build chunk. One mesh build already costs far more than a
// job dispatch, so a chunk of one keeps every core busy on uneven inputs.
inline constexpr uint32 kMeshBuildBatchSize = 1u;

// Builds the stackless node array described in ThreadedBvh.h from a triangle
// soup, partitioning with a binned surface-area heuristic.
//
// The threaded form is emitted DIRECTLY: nodes are appended in pre-order as the
// partition descends, and a node's miss link is patched once its subtree is
// complete. There is no intermediate pointer tree and no flatten pass.
class ThreadedBvhBuilder
{
public:
    // Returns an empty ThreadedBvh when the soup has no triangles, when an
    // index points outside the vertex array, or when the triangle/node count
    // would exceed the format's 24-bit ceilings. Every rejection is logged.
    static ThreadedBvh Build(const MeshGeometryView& soup);

    // One independent build per mesh, fanned out across the pool. `outBvhs`
    // must already be sized to match `soups`; results land at matching indices.
    // A null or shutting-down pool falls back to a sequential build.
    static void BuildMany(JobSystem::WorkStealingThreadPool* pool,
                          std::span<const MeshGeometryView> soups,
                          std::span<ThreadedBvh> outBvhs);

private:
    // AABB of a triangle range plus the AABB of that range's centroids. The
    // first becomes the node's stored bounds, the second drives binning.
    struct RangeBounds
    {
        Mathematics::AABB Node;
        Mathematics::AABB Centroid;
    };

    explicit ThreadedBvhBuilder(const MeshGeometryView& soup);

    bool PrepareTriangles();
    bool Partition();
    ThreadedBvh Emit();

    RangeBounds ComputeRangeBounds(uint32 lo, uint32 hi) const;

    // Partitions m_Order[lo, hi) in place and returns the split position, or
    // `hi` when no binned split beats the cost of a leaf.
    uint32 ChooseSahSplit(uint32 lo, uint32 hi, const RangeBounds& bounds);

    // Fallback when the SAH declines or the centroids are degenerate: split the
    // range at its median along the widest centroid axis.
    uint32 MedianSplit(uint32 lo, uint32 hi, const Mathematics::AABB& centroidBounds);

    uint32 AppendNode(const Mathematics::AABB& bounds);

    const MeshGeometryView& m_Soup;
    uint32 m_TriangleCount = 0;
    uint32 m_VertexCount = 0;

    // SoA scratch indexed by SOURCE triangle id.
    std::vector<float32> m_TriangleBounds;    // 6 per triangle
    std::vector<float32> m_TriangleCentroids; // 3 per triangle

    // Permutation partitioned in place; m_Order[slot] = source triangle id.
    std::vector<uint32> m_Order;

    // Node array under construction, already in the final GPU layout.
    std::vector<uint32> m_Nodes;
};

} // namespace GameEngine::SceneBvh
