#pragma once

#include <span>

#include "Mathematics/Geometry.h"
#include "Types/Types.h"

namespace GameEngine::SceneBvh
{

// The engine's one binned-SAH split kernel, shared by every CPU BVH builder —
// ThreadedBvhBuilder (the GPU software-trace format) and MeshPicking::MeshBvh
// (the CWBVH8 picking format) both partition with this. The kernel decides the
// best (axis, bin) split for a triangle range; what a consumer DOES with that
// decision — leaf policy, node emission, partition routine — stays its own,
// which is why the two builders can produce their existing tree shapes while
// sharing the scoring.
//
// Bins per axis: 16 is the standard sweet spot — split quality flattens out
// past ~16 bins while the binning pass keeps costing more.
inline constexpr uint32 kBinnedSahBinCount = 16u;

// Bin index of a centroid coordinate, clamped into [0, kBinnedSahBinCount).
// `scale` is kBinnedSahBinCount / centroid-extent on that axis. Exposed so a
// consumer's partition predicate assigns bins EXACTLY as the scorer did.
inline uint32 BinnedSahBinOf(float32 centroid, float32 axisMin, float32 scale)
{
    const int32 bin = static_cast<int32>((centroid - axisMin) * scale);
    return static_cast<uint32>(
        bin < 0 ? 0 : (bin >= static_cast<int32>(kBinnedSahBinCount)
                           ? static_cast<int32>(kBinnedSahBinCount) - 1
                           : bin));
}

// One triangle range, described by whatever arrays the consumer already keeps.
//
// Centroids are CALLER-DEFINED on purpose: ThreadedBvhBuilder bins on the
// AABB midpoint while MeshBvh bins on the vertex mean, and both definitions
// are load-bearing for their existing tree shapes. The kernel only requires
// that CentroidBounds bound the same values the Centroids array holds.
struct BinnedSahRange
{
    // Triangle ids in the range, in the consumer's current partition order.
    std::span<const uint32> Order;
    // 3 floats per triangle id (indexed by id, not by range position).
    std::span<const float32> Centroids;
    // 6 floats per triangle id: min xyz, max xyz.
    std::span<const float32> TriangleBounds;
    // Centroid bounds over the range.
    Mathematics::AABB CentroidBounds;
};

struct BinnedSahCandidate
{
    // False when no axis had centroid extent or no split left both sides
    // non-empty; consumers fall back to their own degenerate policy.
    bool Found = false;
    uint32 Axis = 3u;
    uint32 Bin = 0u;   // triangles with bin <= Bin on Axis go left
    float32 AxisMin = 0.0f;
    float32 Scale = 0.0f;  // feed both back into BinnedSahBinOf to partition
    // Al*Nl + Ar*Nr with FULL surface areas — the un-normalised SAH numerator.
    // Consumers apply their own cost model around it (ThreadedBvhBuilder
    // normalises by parent area and compares against the leaf cost; MeshBvh
    // splits whenever it is above its leaf size).
    float32 Cost = 0.0f;
};

// Scores all (axis, bin) candidates over the range and returns the argmin.
BinnedSahCandidate FindBestBinnedSahSplit(const BinnedSahRange& range);

}  // namespace GameEngine::SceneBvh
