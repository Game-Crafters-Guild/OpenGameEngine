#pragma once

// Pure geometry for point-light cube-shadow face culling against the main
// camera (arc slice S1). Two decisions, both keep-biased (they never reject a
// face/light that could affect a visible pixel):
//
//   1. Light level  — is the light's range sphere potentially inside the view?
//                      An out-of-range / off-screen light declares NO faces.
//   2. Per face     — can face f's 90° cube-face frustum affect any visible
//                      fragment? A shaded fragment samples EXACTLY ONE cube
//                      face (largest-axis of light->fragment), so a face whose
//                      frustum cannot intersect the camera frustum can never be
//                      the sampled face for an on-screen pixel — skipping it is
//                      free correctness (design §4.3), provided the test is
//                      conservative.
//
// The per-face test is a conservative separating-axis test over the two
// frustums' FACE-NORMAL axes only (skipping edge-cross axes). SAT proves two
// convex hulls disjoint iff SOME axis in {face normals of A, face normals of B,
// edge cross products} separates them; testing a subset can only ever fail to
// find a separation that an edge axis would have caught — i.e. it may KEEP a
// truly-separated face (safe) but never CULLS an intersecting one. That is the
// keep-bias the design mandates ("full SAT or a conservative approximation;
// naive corner tests over-cull").
//
// Everything here is header-declared / cpp-defined pure math with no device or
// RenderServices state, so PopulatePointShadowGeometry computes the 6-bit mask
// ONCE (design A10) and the headless unit tests exercise it directly.

#include "Mathematics/Vector3.h"
#include "Mathematics/Vector4.h"

#include <array>
#include <cstdint>

namespace GameEngine
{
namespace Engine::Renderer
{

// Receiver-side normal bias applied when sampling the point-shadow map —
// ShadowMapRenderFeature writes it into PointShadowSlotGPU.pointShadowParams[3],
// and it offsets the sampled fragment position along its surface normal to fight
// self-shadow acne. The face-cull keep-bias margin floor (KeepBiasMargin in the
// .cpp) MUST stay strictly above this: a fragment nudged by up to this much
// could otherwise land on a face the SAT culled, and the offset sample would
// read that face's unrendered (cleared-far) layer — a missing shadow. The .cpp
// static_asserts the floor > this value so the two never drift apart.
inline constexpr float kPointShadowNormalBias = 0.02f;

// Camera-side inputs for the face-cull tests, all in full world space (the
// space ResolveCameraData's view/proj live in — camera-relative rebasing lives
// in separate CameraData fields and is irrelevant for local point lights).
// Built once per view by RenderServices::MakePointShadowCullInputs and shared
// by the declaration path and the GPU cull scheduler so both agree (no drift).
struct PointShadowCameraCull
{
    Mathematics::Vector3 CameraPosition{};
    // Inward-pointing, normalized (Rendering::ExtractFrustumPlanes order:
    // L, R, B, T, Near, Far). A point P is inside plane p when
    // p.x*P.x + p.y*P.y + p.z*P.z + p.w >= 0.
    std::array<Mathematics::Vector4, 6> FrustumPlanes{};
    // World-space frustum corners (ExtractFrustumCornersWS order: 4 near then
    // 4 far). Used for the face-plane half of the conservative SAT.
    std::array<Mathematics::Vector3, 8> FrustumCorners{};
};

// True when the light's range sphere is at least partially inside the camera
// frustum (keep-biased: the underlying TestSphereFrustum inflates the radius).
// A false result means the whole light can be dropped — it stays lit, just
// unshadowed.
bool PointShadowLightVisible(const Mathematics::Vector3& lightPositionWS, float range,
                             const PointShadowCameraCull& cam);

// 6-bit visibility mask (bit f set => face f must be rendered). Face order
// matches PopulatePointShadowGeometry / GE_PointShadowFace: 0:+X 1:-X 2:+Y 3:-Y
// 4:+Z 5:-Z. Keep-biased. Returns all-faces (0x3F) when the camera is inside
// the light's range volume (a nearby fragment can sample any face). The per-face
// frustum spans from the light apex (near = 0) so a visible fragment arbitrarily
// close to the light centre can never fall on a culled face.
uint8_t ComputePointShadowFaceMask(const Mathematics::Vector3& lightPositionWS, float range,
                                   const PointShadowCameraCull& cam);

} // namespace Engine::Renderer
} // namespace GameEngine
