#include "SceneBvh/BinnedSahSplit.h"

#include <array>
#include <limits>

namespace GameEngine::SceneBvh
{
using Mathematics::AABB;

BinnedSahCandidate FindBestBinnedSahSplit(const BinnedSahRange& range)
{
    BinnedSahCandidate best;
    best.Cost = std::numeric_limits<float32>::max();

    for (uint32 axis = 0; axis < 3u; ++axis)
    {
        const float32 axisMin = range.CentroidBounds.min[static_cast<int>(axis)];
        const float32 axisMax = range.CentroidBounds.max[static_cast<int>(axis)];
        const float32 extent = axisMax - axisMin;
        if (extent <= 0.0f)
            continue;

        const float32 scale = static_cast<float32>(kBinnedSahBinCount) / extent;

        std::array<AABB, kBinnedSahBinCount> binBounds;
        std::array<uint32, kBinnedSahBinCount> binCounts{};
        binBounds.fill(AABB::Empty());

        for (const uint32 tri : range.Order)
        {
            const uint32 bin = BinnedSahBinOf(range.Centroids[tri * 3u + axis], axisMin, scale);
            ++binCounts[bin];
            const float32* triBounds = &range.TriangleBounds[static_cast<size_t>(tri) * 6u];
            binBounds[bin].Expand(AABB{{triBounds[0], triBounds[1], triBounds[2]},
                                       {triBounds[3], triBounds[4], triBounds[5]}});
        }

        // Suffix sweep first: rightArea[b] / rightCount[b] describe bins
        // (b, kBinnedSahBinCount).
        std::array<float32, kBinnedSahBinCount - 1u> rightArea{};
        std::array<uint32, kBinnedSahBinCount - 1u> rightCount{};
        AABB accumulated = AABB::Empty();
        uint32 accumulatedCount = 0;
        for (uint32 bin = kBinnedSahBinCount - 1u; bin >= 1u; --bin)
        {
            accumulated.Expand(binBounds[bin]);
            accumulatedCount += binCounts[bin];
            rightArea[bin - 1u] = accumulated.SurfaceArea();
            rightCount[bin - 1u] = accumulatedCount;
        }

        // Prefix sweep: split after bin `b` puts bins [0, b] left, (b, end)
        // right.
        accumulated = AABB::Empty();
        accumulatedCount = 0;
        for (uint32 bin = 0; bin + 1u < kBinnedSahBinCount; ++bin)
        {
            accumulated.Expand(binBounds[bin]);
            accumulatedCount += binCounts[bin];
            if (accumulatedCount == 0 || rightCount[bin] == 0)
                continue;

            const float32 cost = accumulated.SurfaceArea() * static_cast<float32>(accumulatedCount) +
                                 rightArea[bin] * static_cast<float32>(rightCount[bin]);
            if (cost < best.Cost)
            {
                best.Found = true;
                best.Axis = axis;
                best.Bin = bin;
                best.AxisMin = axisMin;
                best.Scale = scale;
                best.Cost = cost;
            }
        }
    }

    return best;
}

}  // namespace GameEngine::SceneBvh
