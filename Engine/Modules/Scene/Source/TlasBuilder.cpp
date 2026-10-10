#include "TlasInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace GameEngine::Scene::Internal
{

namespace
{

inline Vector3 BoundsCenter(const AABB& b)
{
    return Vector3{(b.min.x + b.max.x) * 0.5f,
                   (b.min.y + b.max.y) * 0.5f,
                   (b.min.z + b.max.z) * 0.5f};
}

inline float32 AxisOf(const Vector3& v, int axis)
{
    return axis == 0 ? v.x : (axis == 1 ? v.y : v.z);
}

struct SahBin
{
    AABB   Bounds = AABB::Empty();
    uint32 Count  = 0u;
};

// Recursive top-down build over a range of leaf indices in
// `leafIndices[first..first+count)`. Returns the index of the node it
// created in sector.Nodes.
uint32 BuildRecursive(SectorState& sector,
                      std::vector<uint32>& leafIndices,
                      uint32 first,
                      uint32 count,
                      uint32 parentNode)
{
    const uint32 nodeIdx = static_cast<uint32>(sector.Nodes.size());
    sector.Nodes.push_back({});
    auto& node = sector.Nodes[nodeIdx];
    node.Parent = parentNode;

    // Compute combined leaf bounds and centroid bounds.
    AABB bounds  = AABB::Empty();
    AABB cbounds = AABB::Empty();
    for (uint32 i = 0; i < count; ++i)
    {
        const auto& leaf = sector.Leaves[leafIndices[first + i]];
        bounds.Expand(leaf.WorldBounds);
        cbounds.Expand(BoundsCenter(leaf.WorldBounds));
    }
    sector.Nodes[nodeIdx].Bounds = bounds;

    if (count <= kLeafCapacity)
    {
        sector.Nodes[nodeIdx].FirstLeaf = first;  // re-interpret: range start in leafIndices
        sector.Nodes[nodeIdx].LeafCount = count;
        for (uint32 i = 0; i < count; ++i)
            sector.Leaves[leafIndices[first + i]].ParentNode = nodeIdx;
        return nodeIdx;
    }

    // Binned SAH across all 3 axes.
    int     bestAxis     = -1;
    int     bestSplitBin = 0;
    float32 bestCost     = std::numeric_limits<float32>::max();

    for (int axis = 0; axis < 3; ++axis)
    {
        const float32 axisMin = AxisOf(cbounds.min, axis);
        const float32 axisMax = AxisOf(cbounds.max, axis);
        const float32 extent  = axisMax - axisMin;
        if (extent <= 0.0f)
            continue;

        SahBin bins[kSahBins];
        const float32 invExtent = static_cast<float32>(kSahBins) / extent;
        for (uint32 i = 0; i < count; ++i)
        {
            const auto& leaf = sector.Leaves[leafIndices[first + i]];
            const Vector3 c = BoundsCenter(leaf.WorldBounds);
            int b = static_cast<int>((AxisOf(c, axis) - axisMin) * invExtent);
            if (b < 0) b = 0;
            if (b >= static_cast<int>(kSahBins)) b = kSahBins - 1;
            bins[b].Count++;
            bins[b].Bounds.Expand(leaf.WorldBounds);
        }

        uint32  leftCount[kSahBins]  = {};
        uint32  rightCount[kSahBins] = {};
        float32 leftArea[kSahBins]   = {};
        float32 rightArea[kSahBins]  = {};

        AABB   acc      = AABB::Empty();
        uint32 accCount = 0u;
        for (uint32 i = 0; i < kSahBins; ++i)
        {
            accCount += bins[i].Count;
            if (bins[i].Count > 0u) acc.Expand(bins[i].Bounds);
            leftCount[i] = accCount;
            leftArea[i]  = (accCount > 0u) ? acc.SurfaceArea() : 0.0f;
        }
        acc      = AABB::Empty();
        accCount = 0u;
        for (int i = static_cast<int>(kSahBins) - 1; i >= 0; --i)
        {
            accCount += bins[i].Count;
            if (bins[i].Count > 0u) acc.Expand(bins[i].Bounds);
            rightCount[i] = accCount;
            rightArea[i]  = (accCount > 0u) ? acc.SurfaceArea() : 0.0f;
        }

        for (int i = 0; i < static_cast<int>(kSahBins) - 1; ++i)
        {
            const uint32 lc = leftCount[i];
            const uint32 rc = rightCount[i + 1];
            if (lc == 0u || rc == 0u)
                continue;
            const float32 cost = static_cast<float32>(lc) * leftArea[i]
                               + static_cast<float32>(rc) * rightArea[i + 1];
            if (cost < bestCost)
            {
                bestCost     = cost;
                bestAxis     = axis;
                bestSplitBin = i;
            }
        }
    }

    // Fallback: no valid split (degenerate centroid bounds). Halve by index.
    if (bestAxis < 0)
    {
        const uint32 leftCnt = count / 2u;
        const uint32 left  = BuildRecursive(sector, leafIndices, first, leftCnt, nodeIdx);
        const uint32 right = BuildRecursive(sector, leafIndices, first + leftCnt, count - leftCnt, nodeIdx);
        sector.Nodes[nodeIdx].Left  = left;
        sector.Nodes[nodeIdx].Right = right;
        sector.Nodes[nodeIdx].SplitAxis = 0u;
        return nodeIdx;
    }

    // Partition leafIndices[first..first+count) by chosen split.
    const float32 axisMin   = AxisOf(cbounds.min, bestAxis);
    const float32 axisExt   = AxisOf(cbounds.max, bestAxis) - axisMin;
    const float32 invExtent = static_cast<float32>(kSahBins) / axisExt;
    const int     splitBin  = bestSplitBin;
    const int     axis      = bestAxis;

    auto leftOfSplit = [&](uint32 leafIdx) {
        const Vector3 c = BoundsCenter(sector.Leaves[leafIdx].WorldBounds);
        int b = static_cast<int>((AxisOf(c, axis) - axisMin) * invExtent);
        if (b < 0) b = 0;
        if (b >= static_cast<int>(kSahBins)) b = kSahBins - 1;
        return b <= splitBin;
    };

    uint32 lo = first;
    uint32 hi = first + count - 1u;
    while (lo <= hi)
    {
        if (leftOfSplit(leafIndices[lo]))
        {
            ++lo;
        }
        else
        {
            std::swap(leafIndices[lo], leafIndices[hi]);
            if (hi == 0u) break;
            --hi;
        }
    }
    uint32 leftCount = lo - first;
    if (leftCount == 0u || leftCount == count)
        leftCount = count / 2u;

    const uint32 left  = BuildRecursive(sector, leafIndices, first, leftCount, nodeIdx);
    const uint32 right = BuildRecursive(sector, leafIndices, first + leftCount, count - leftCount, nodeIdx);
    sector.Nodes[nodeIdx].Left      = left;
    sector.Nodes[nodeIdx].Right     = right;
    sector.Nodes[nodeIdx].SplitAxis = static_cast<uint8>(bestAxis);
    return nodeIdx;
}

}  // anon namespace

void BuildFromLeaves(SectorState& sector)
{
    sector.Nodes.clear();

    if (sector.Leaves.empty())
        return;

    // Thread-local scratches: clear() preserves capacity so successive
    // sector rebuilds in the same Sync amortize allocation. BuildFromLeaves
    // is called only under SceneTlasImpl::Mutex's unique_lock so no
    // concurrent access; nesting isn't possible (no caller invokes BVH
    // build from inside another).
    thread_local std::vector<uint32>   tl_LeafIndices;
    thread_local std::vector<TlasLeaf> tl_Reordered;
    thread_local std::vector<uint32>   tl_OldToNew;

    // We keep sector.Leaves in a stable order across builds (so EntityToLeaf
    // entries don't shift). Build with a permutation array of leaf indices.
    tl_LeafIndices.clear();
    tl_LeafIndices.reserve(sector.Leaves.size());
    for (uint32 i = 0; i < sector.Leaves.size(); ++i)
        tl_LeafIndices.push_back(i);

    sector.Nodes.reserve(sector.Leaves.size() * 2u);
    BuildRecursive(sector, tl_LeafIndices, 0u, static_cast<uint32>(sector.Leaves.size()), ~0u);

    // Repack sector.Leaves so leaf-node ranges are contiguous in the array.
    // BuildRecursive wrote node.FirstLeaf as a range-start in leafIndices;
    // we reorder the leaves to match that range layout, then update each
    // leaf's ParentNode to the same value.
    tl_Reordered.clear();
    tl_Reordered.reserve(sector.Leaves.size());
    tl_OldToNew.assign(sector.Leaves.size(), ~0u);
    for (uint32 i = 0; i < tl_LeafIndices.size(); ++i)
    {
        tl_OldToNew[tl_LeafIndices[i]] = i;
        tl_Reordered.push_back(std::move(sector.Leaves[tl_LeafIndices[i]]));
    }
    sector.Leaves.swap(tl_Reordered);
    // EntityToLeaf maps EntityId -> old leaf index; remap to new.
    for (auto& kv : sector.EntityToLeaf)
        kv.second = tl_OldToNew[kv.second];
}

}  // namespace GameEngine::Scene::Internal
