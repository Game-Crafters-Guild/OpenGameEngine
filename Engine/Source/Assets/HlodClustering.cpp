#include "Assets/HlodClustering.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace GameEngine {
namespace Hlod {

namespace {

// floor() into an int32 cell index. std::floor keeps negative coordinates
// single-owner (e.g. -0.1 → cell -1, not 0) so cells tile world space without
// a seam at the origin.
int32 FloorToCell(float coord, float origin, float cellSize) {
    return static_cast<int32>(std::floor((coord - origin) / cellSize));
}

} // namespace

ClusterTable BuildClusters(std::span<const MemberInput> members, const GridConfig& config) {
    ClusterTable table;
    if (config.CellSize <= 0.0f)
        return table;

    // Ordered map so cluster emission is deterministic (cell-coord ascending)
    // regardless of member order; member indices land ascending because members
    // are visited in index order.
    std::map<CellCoord, std::vector<uint32>> cells;

    for (uint32 i = 0; i < static_cast<uint32>(members.size()); ++i) {
        const MemberInput& m = members[i];

        const float extentX = m.WorldAabbMax[0] - m.WorldAabbMin[0];
        const float extentY = m.WorldAabbMax[1] - m.WorldAabbMin[1];
        const float extentZ = m.WorldAabbMax[2] - m.WorldAabbMin[2];
        if (extentX > config.CellSize || extentY > config.CellSize || extentZ > config.CellSize) {
            // Larger than one cell: single-owner-by-center would give it a
            // misleading footprint, and merging it risks double geometry across
            // an abutting cell. Draw it directly (caller warns).
            table.OversizedMembers.push_back(i);
            continue;
        }

        const float centerX = (m.WorldAabbMin[0] + m.WorldAabbMax[0]) * 0.5f;
        const float centerY = (m.WorldAabbMin[1] + m.WorldAabbMax[1]) * 0.5f;
        const float centerZ = (m.WorldAabbMin[2] + m.WorldAabbMax[2]) * 0.5f;

        CellCoord cell{
            FloorToCell(centerX, config.GridOrigin[0], config.CellSize),
            FloorToCell(centerY, config.GridOrigin[1], config.CellSize),
            FloorToCell(centerZ, config.GridOrigin[2], config.CellSize)};
        cells[cell].push_back(i);
    }

    table.Clusters.reserve(cells.size());
    for (auto& [cell, memberIndices] : cells) {
        Cluster cluster;
        cluster.Cell = cell;
        cluster.MemberIndices = std::move(memberIndices);

        float minB[3] = {std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max(),
                         std::numeric_limits<float>::max()};
        float maxB[3] = {std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest(),
                         std::numeric_limits<float>::lowest()};
        for (uint32 mi : cluster.MemberIndices) {
            const MemberInput& m = members[mi];
            for (int c = 0; c < 3; ++c) {
                minB[c] = std::min(minB[c], m.WorldAabbMin[c]);
                maxB[c] = std::max(maxB[c], m.WorldAabbMax[c]);
            }
        }
        float radiusSq = 0.0f;
        for (int c = 0; c < 3; ++c) {
            cluster.BoundsMin[c] = minB[c];
            cluster.BoundsMax[c] = maxB[c];
            cluster.SphereCenter[c] = (minB[c] + maxB[c]) * 0.5f;
            const float half = (maxB[c] - minB[c]) * 0.5f;
            radiusSq += half * half;
        }
        cluster.SphereRadius = std::sqrt(radiusSq);
        table.Clusters.push_back(std::move(cluster));
    }

    return table;
}

} // namespace Hlod
} // namespace GameEngine
