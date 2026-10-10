#pragma once

#include "Rendering/CameraTypes.h"
#include "Rendering/Common/Math.h"

namespace GameEngine
{
namespace Rendering
{

// All per-frame camera quantities downstream culling / LOD / debug code
// needs in one struct. Lives alongside CameraData (the GPU-uploaded layout)
// rather than on it because the derivation pulls in Mathematics types we
// don't want every CameraData consumer to drag in.
struct CameraDerivedData
{
    Matrix4x4 ViewMatrix;
    Matrix4x4 ProjMatrix;
    Matrix4x4 ViewProjMatrix;
    Vector3 Position;
    Vector3 Forward;
    float NearPlane = 0.1f;
    float FarPlane = 1000.0f;
    // Vertical screen-space slope of the projection. Unifies perspective
    // and orthographic LOD math:
    //   perspective: ScreenScale = tan(fovY/2)       (size shrinks with distance)
    //   ortho:       ScreenScale = orthoHalfHeight   (size independent of distance)
    // Derived from the projection's m[1][1] entry. 1.0 if the matrix degenerates.
    float ScreenScale = 1.0f;
    // True when ProjMatrix is orthographic (homogeneous-w row == 1).
    bool IsOrthographic = false;
};

// Single source of truth for the camera-math any per-view system needs.
// CPU-side derivations only — does not extract frustum planes (that's
// `Rendering::ExtractFrustumPlanes(viewProj, out)` and lives in
// Common/Frustum.h). Reverse-Z near/far recovery is conservative: if the
// matrix doesn't look like an `MakePerspectiveLH_ZO_ReverseZ` output (e.g.
// orthographic) the function leaves the default near/far in place.
CameraDerivedData DeriveCameraData(const CameraData& source);

} // namespace Rendering
} // namespace GameEngine
