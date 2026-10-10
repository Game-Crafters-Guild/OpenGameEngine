#pragma once

// Pure CPU frustum culling for the sorted transparent path, factored out of
// RenderServices so it can be unit-tested with mock spheres and no GPU state.
// (The back-to-front ORDERING lives on the GPU as of transparency-scale S2 —
// sorted_transparent_drain.comp — with BuildSortedTransparentCpuOrder in
// SortedTransparentRuns.h as the past-capacity CPU twin.)

#include "Rendering/Common/Frustum.h" // TestSphereFrustum, Vector4
#include "Rendering/Core/GPUCulling.h" // MakeFrustumPlanesCameraRelative

#include <cstdint>
#include <vector>

namespace GameEngine::Engine::Renderer
{

// One visible transparent instance's world-space centre plus an opaque caller
// key (an index the caller uses to recover the record after the cull).
struct TransparentCentroid
{
    uint32_t Key;
    float Center[3];
};

// A transparent draw candidate before culling: its GPUScene world-space
// bounding sphere plus the same opaque caller key TransparentCentroid carries.
// Center / Radius are copied straight off GPUInstance.boundingCenter /
// boundingRadius so the CPU cull here and the GPU frustum-culling compute
// (frustum_culling.comp) test byte-identical sphere data and agree on the
// visible set.
struct TransparentSphere
{
    uint32_t Key;
    float Center[3];
    float Radius;
};

// Append the frustum-VISIBLE subset of `candidates` to `outVisible` (as
// TransparentCentroids carrying each survivor's Key + Center). `worldPlanes`
// is the 6 inward-pointing view-frustum planes from
// Rendering::ExtractFrustumPlanes (world space); `origin` is the view's
// camera-relative render origin (RenderServicesGpuDriven derives it from the
// camera position via ComputeRenderOriginSector — (0,0,0) whenever
// camera-relative rendering is inactive).
//
// Camera-relative rebase (Earth-scale precision, sector-tagged transparents):
// the planes are translated by the origin and each centre is differenced
// against it before the test — the EXACT arithmetic the GPU cull performs
// (MakeFrustumPlanesCameraRelative + frustum_culling.comp's
// `boundingCenter - cameraPosition`), so an instance moving between the
// batched (GPU-culled) and sorted (CPU-culled) paths never flips its cull
// decision. At origin (0,0,0) both steps are bit-for-bit no-ops and this is
// byte-identical to the plain world-space test.
//
// The test is the shared Rendering::TestSphereFrustum, which inflates the
// radius 1.5x internally — the same kCullMarginConservative the main-view GPU
// cull applies — so the decision is deliberately biased toward keeping: a
// false accept costs one extra sorted draw, while a false reject would pop a
// visible transparent at a screen edge (unacceptable).
//
// Returns the number culled (for diagnostics).
inline uint32_t CullTransparentsToFrustum(const std::vector<TransparentSphere>& candidates,
                                          const Rendering::Vector4* worldPlanes,
                                          const Rendering::Vector3& origin,
                                          std::vector<TransparentCentroid>& outVisible)
{
    Rendering::Vector4 planes[6];
    for (int p = 0; p < 6; ++p)
        planes[p] = worldPlanes[p];
    Rendering::MakeFrustumPlanesCameraRelative(planes, origin);

    uint32_t culled = 0;
    for (const TransparentSphere& c : candidates)
    {
        const Rendering::Vector3 center(c.Center[0] - origin.x, c.Center[1] - origin.y,
                                        c.Center[2] - origin.z);
        if (!Rendering::TestSphereFrustum(center, c.Radius, planes))
        {
            ++culled;
            continue;
        }
        outVisible.push_back({c.Key, {c.Center[0], c.Center[1], c.Center[2]}});
    }
    return culled;
}

} // namespace GameEngine::Engine::Renderer
