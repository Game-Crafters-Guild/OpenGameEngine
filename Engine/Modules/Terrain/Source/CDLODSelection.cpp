#include "Terrain/CDLODSelection.h"
#include "Terrain/CDLODQuadtree.h"

#include <cassert>
#include <cmath>

namespace GameEngine::Terrain
{

float32 CDLODSelection::ComputeLODRange(uint32 level, float32 baseRange, float32 rangeScale)
{
    float32 range = baseRange;
    for (uint32 i = 0; i < level; ++i)
    {
        range *= rangeScale;
    }
    return range;
}

void CDLODSelection::Select(const CDLODQuadtree& quadtree,
                             const CDLODSelectionParams& params,
                             std::vector<CDLODPatch>& outPatches) const
{
    if (!quadtree.IsBuilt())
        return;

    const uint32 numLevels = quadtree.GetNumLevels();
    if (numLevels == 0)
        return;

    const uint32 finestNodesPerAxis = quadtree.GetNodesPerAxisAtLevel(0);
    const float32 patchSizeX = params.WorldSizeX / static_cast<float32>(finestNodesPerAxis);
    const float32 patchSizeZ = params.WorldSizeZ / static_cast<float32>(finestNodesPerAxis);

    // Use pre-computed ranges if provided, otherwise compute them.
    float32 visRangesLocal[kMaxLODLevels] = {};
    float32 morphStartLocal[kMaxLODLevels] = {};
    float32 morphEndLocal[kMaxLODLevels] = {};

    const float32* visRanges = params.PrecomputedVisRanges;
    const float32* morphStartArr = params.PrecomputedMorphStart;
    const float32* morphEndArr = params.PrecomputedMorphEnd;

    if (!visRanges || !morphStartArr || !morphEndArr)
    {
        const float32 baseRange = std::max(patchSizeX, patchSizeZ) * params.LODRangeScale;
        constexpr float32 kMorphStartRatio = 0.66f;

        for (uint32 i = 0; i < numLevels; ++i)
            visRangesLocal[i] = ComputeLODRange(i, baseRange, params.LODRangeScale);

        float32 prevMorphStart = 0.0f;
        for (uint32 i = 0; i < numLevels; ++i)
        {
            morphEndLocal[i] = visRangesLocal[i];
            morphStartLocal[i] = prevMorphStart + (morphEndLocal[i] - prevMorphStart) * kMorphStartRatio;
            prevMorphStart = morphStartLocal[i];
        }

        visRanges = visRangesLocal;
        morphStartArr = morphStartLocal;
        morphEndArr = morphEndLocal;
    }

    const uint32 rootLevel = numLevels - 1;
    const uint32 rootNodesPerAxis = quadtree.GetNodesPerAxisAtLevel(rootLevel);

    for (uint32 z = 0; z < rootNodesPerAxis; ++z)
    {
        for (uint32 x = 0; x < rootNodesPerAxis; ++x)
        {
            const uint32 finestPerRoot = finestNodesPerAxis / rootNodesPerAxis;
            const float32 nodeWorldX = params.WorldOriginX + static_cast<float32>(x * finestPerRoot) * patchSizeX;
            const float32 nodeWorldZ = params.WorldOriginZ + static_cast<float32>(z * finestPerRoot) * patchSizeZ;
            const float32 nodeWorldSizeX = static_cast<float32>(finestPerRoot) * patchSizeX;
            const float32 nodeWorldSizeZ = static_cast<float32>(finestPerRoot) * patchSizeZ;

            SelectRecursive(quadtree, params, rootLevel, x, z,
                            nodeWorldX, nodeWorldZ,
                            nodeWorldSizeX, nodeWorldSizeZ,
                            patchSizeX, patchSizeZ,
                            visRanges, morphStartArr, morphEndArr,
                            outPatches);
        }
    }
}

// Returns true if this node (or its children) were selected for rendering.
bool CDLODSelection::SelectRecursive(const CDLODQuadtree& quadtree,
                                      const CDLODSelectionParams& params,
                                      uint32 level, uint32 nodeX, uint32 nodeZ,
                                      float32 nodeWorldX, float32 nodeWorldZ,
                                      float32 nodeWorldSizeX, float32 nodeWorldSizeZ,
                                      float32 patchSizeX, float32 patchSizeZ,
                                      const float32* visRanges,
                                      const float32* morphStartArr,
                                      const float32* morphEndArr,
                                      std::vector<CDLODPatch>& outPatches) const
{
    const auto& node = quadtree.GetNode(level, nodeX, nodeZ);

    // Frustum culling (C-#4: use separate X/Z extents for non-square support)
    if (params.FrustumPlanes)
    {
        const float32 scaledMin = node.MinHeight * params.HeightScale;
        const float32 scaledMax = node.MaxHeight * params.HeightScale;

        if (!IsNodeInFrustum(params.FrustumPlanes,
                             nodeWorldX, nodeWorldZ,
                             nodeWorldSizeX, nodeWorldSizeZ,
                             scaledMin, scaledMax, params.WorldOriginY))
        {
            return false;
        }
    }

    const float32 lodRange = visRanges[level];

    // Use AABB-sphere intersection for LOD decision (ref: Strugar CDLOD).
    // This checks whether the camera is within lodRange of ANY point on the
    // node's bounding box, not just the center. This prevents popping when
    // a large node's center is far but its near edge is close.
    const float32 nodeMinX = nodeWorldX;
    const float32 nodeMaxX = nodeWorldX + nodeWorldSizeX;
    const float32 nodeMinZ = nodeWorldZ;
    const float32 nodeMaxZ = nodeWorldZ + nodeWorldSizeZ;

    // Closest point on AABB to camera (XZ only for LOD, ignoring Y)
    const float32 closestX = std::max(nodeMinX, std::min(params.CameraPosition.x, nodeMaxX));
    const float32 closestZ = std::max(nodeMinZ, std::min(params.CameraPosition.z, nodeMaxZ));
    const float32 dx = closestX - params.CameraPosition.x;
    const float32 dz = closestZ - params.CameraPosition.z;
    const float32 minDistSq = dx * dx + dz * dz;

    // Out of range: closest point on AABB is beyond lodRange
    if (minDistSq > lodRange * lodRange)
        return false;

    // At finest level, always emit full patch
    if (level == 0)
    {
        CDLODPatch patch{};
        patch.WorldX = nodeWorldX;
        patch.WorldZ = nodeWorldZ;
        patch.Scale = patchSizeX;
        patch.LODLevel = level;
        patch.MorphStartDist = morphStartArr[level];
        patch.MorphEndDist = morphEndArr[level];
        patch.SubQuadFlags = 0xF; // all 4 quadrants
        outPatches.push_back(patch);
        return true;
    }

    // Try to subdivide to children
    const uint32 childLevel = level - 1;
    const float32 childSizeX = nodeWorldSizeX * 0.5f;
    const float32 childSizeZ = nodeWorldSizeZ * 0.5f;

    bool childSelected[4] = {false, false, false, false};

    for (uint32 cz = 0; cz < 2; ++cz)
    {
        for (uint32 cx = 0; cx < 2; ++cx)
        {
            const uint32 childNodeX = nodeX * 2 + cx;
            const uint32 childNodeZ = nodeZ * 2 + cz;
            const float32 childWorldX = nodeWorldX + static_cast<float32>(cx) * childSizeX;
            const float32 childWorldZ = nodeWorldZ + static_cast<float32>(cz) * childSizeZ;

            if (childNodeX < quadtree.GetNodesPerAxisAtLevel(childLevel) &&
                childNodeZ < quadtree.GetNodesPerAxisAtLevel(childLevel))
            {
                childSelected[cz * 2 + cx] = SelectRecursive(
                    quadtree, params, childLevel, childNodeX, childNodeZ,
                    childWorldX, childWorldZ,
                    childSizeX, childSizeZ,
                    patchSizeX, patchSizeZ,
                    visRanges, morphStartArr, morphEndArr,
                    outPatches);
            }
        }
    }

    // If ALL children were selected, this node doesn't need to render anything
    if (childSelected[0] && childSelected[1] && childSelected[2] && childSelected[3])
        return true;

    // If NO children were selected, render full patch at this level
    // If SOME children were selected, render only the unoccupied sub-quadrants
    uint32 subFlags = 0;
    if (!childSelected[0]) subFlags |= 0x1; // TL
    if (!childSelected[1]) subFlags |= 0x2; // TR
    if (!childSelected[2]) subFlags |= 0x4; // BL
    if (!childSelected[3]) subFlags |= 0x8; // BR

    if (subFlags != 0)
    {
        CDLODPatch patch{};
        patch.WorldX = nodeWorldX;
        patch.WorldZ = nodeWorldZ;
        patch.Scale = nodeWorldSizeX;
        patch.LODLevel = level;
        patch.MorphStartDist = morphStartArr[level];
        patch.MorphEndDist = morphEndArr[level];
        patch.SubQuadFlags = subFlags;
        outPatches.push_back(patch);
    }

    return true;
}

bool CDLODSelection::IsNodeInFrustum(const float32* frustumPlanes,
                                      float32 nodeWorldX, float32 nodeWorldZ,
                                      float32 sizeX, float32 sizeZ,
                                      float32 minHeight, float32 maxHeight,
                                      float32 worldOriginY) const
{
    const float32 cx = nodeWorldX + sizeX * 0.5f;
    const float32 cy = worldOriginY + (minHeight + maxHeight) * 0.5f;
    const float32 cz = nodeWorldZ + sizeZ * 0.5f;
    const float32 hx = sizeX * 0.5f;
    const float32 hy = (maxHeight - minHeight) * 0.5f;
    const float32 hz = sizeZ * 0.5f;

    for (uint32 i = 0; i < 6; ++i)
    {
        const float32 nx = frustumPlanes[i * 4 + 0];
        const float32 ny = frustumPlanes[i * 4 + 1];
        const float32 nz = frustumPlanes[i * 4 + 2];
        const float32 d = frustumPlanes[i * 4 + 3];

        const float32 dist = nx * cx + ny * cy + nz * cz + d;
        const float32 radius = hx * std::abs(nx) + hy * std::abs(ny) + hz * std::abs(nz);

        if (dist + radius < 0.0f)
            return false;
    }

    return true;
}

} // namespace GameEngine::Terrain
