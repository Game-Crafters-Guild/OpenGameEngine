#pragma once

#include "Terrain/TerrainTypes.h"
#include "Mathematics/Vector3.h"

#include <vector>

namespace GameEngine::Terrain
{

class CDLODQuadtree;

// Parameters for CDLOD LOD selection.
struct CDLODSelectionParams
{
    Mathematics::Vector3 CameraPosition;
    float32 WorldSizeX = 1024.0f;
    float32 WorldSizeZ = 1024.0f;
    float32 WorldOriginX = 0.0f;
    float32 WorldOriginZ = 0.0f;
    float32 WorldOriginY = 0.0f;
    float32 HeightScale = 256.0f;
    float32 LODRangeScale = kDefaultLODRangeScale;
    uint32 PatchGridSize = kDefaultGridSize;

    // Pre-computed per-LOD ranges. If non-null, Select() uses these instead
    // of recomputing each frame. All three must be set together or all null.
    const float32* PrecomputedVisRanges = nullptr;
    const float32* PrecomputedMorphStart = nullptr;
    const float32* PrecomputedMorphEnd = nullptr;

    // Frustum planes for culling (6 planes, each as vec4: nx, ny, nz, d).
    // If null, no frustum culling is performed (all visible nodes emitted).
    const float32* FrustumPlanes = nullptr; // pointer to 6 * 4 floats
};

// Performs per-frame CDLOD LOD selection against a quadtree.
// Outputs a list of CDLODPatch structs ready for GPU upload.
class CDLODSelection
{
public:
    CDLODSelection() = default;

    // Run LOD selection. Results are appended to `outPatches`.
    void Select(const CDLODQuadtree& quadtree,
                const CDLODSelectionParams& params,
                std::vector<CDLODPatch>& outPatches) const;

    // Compute the world-space LOD range for a given level.
    // Level 0 = finest (smallest range), increasing with level.
    static float32 ComputeLODRange(uint32 level, float32 baseRange, float32 rangeScale);

private:
    bool SelectRecursive(const CDLODQuadtree& quadtree,
                         const CDLODSelectionParams& params,
                         uint32 level, uint32 nodeX, uint32 nodeZ,
                         float32 nodeWorldX, float32 nodeWorldZ,
                         float32 nodeWorldSizeX, float32 nodeWorldSizeZ,
                         float32 patchSizeX, float32 patchSizeZ,
                         const float32* visRanges,
                         const float32* morphStartArr,
                         const float32* morphEndArr,
                         std::vector<CDLODPatch>& outPatches) const;

    bool IsNodeInFrustum(const float32* frustumPlanes,
                         float32 nodeWorldX, float32 nodeWorldZ,
                         float32 sizeX, float32 sizeZ,
                         float32 minHeight, float32 maxHeight,
                         float32 worldOriginY) const;

};

} // namespace GameEngine::Terrain
