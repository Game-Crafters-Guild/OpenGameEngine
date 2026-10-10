#include "Engine/Rendering/PointShadowFaceCull.h"

#include "Rendering/Common/Frustum.h"

#include <algorithm>
#include <cmath>

namespace GameEngine
{
namespace Engine::Renderer
{
namespace
{
using Mathematics::Vector3;
using Mathematics::Vector4;

inline constexpr uint32_t kFaceCount = 6u;

// Absolute floor of the keep-bias margin for tiny lights. Held strictly above
// the receiver normal bias so a normal-offset sample can never land on a face
// the SAT culled (see kPointShadowNormalBias). The range-scaled term dominates
// for lights larger than 2.5 m; this floor guards the sub-metre torches.
inline constexpr float kFaceCullMarginFloor = 0.05f;
static_assert(kFaceCullMarginFloor > kPointShadowNormalBias,
              "face-cull keep margin floor must exceed the receiver normal bias, "
              "else a normal-biased fragment could sample a culled (cleared-far) face");

// Keep-bias: a separating plane only counts when the opposing hull clears it by
// more than this world margin, so faces grazing the frustum boundary stay
// KEPT (a culled-face pop across the boundary is a correctness bug). Scaled by
// the light range with an absolute floor for tiny lights.
inline float KeepBiasMargin(float range)
{
    return std::max(range * 0.02f, kFaceCullMarginFloor);
}

inline float Dot3(const Vector3& a, const Vector3& b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Signed distance of point c from an inward-pointing plane; >= 0 means inside.
inline float PlaneDistance(const Vector4& p, const Vector3& c)
{
    return p.x * c.x + p.y * c.y + p.z * c.z + p.w;
}

// Shift an inward-pointing plane into a frame translated by `origin`: a plane
// (n, w) with n·x + w >= 0 becomes (n, w + n·origin), since
// n·(x − origin) + (w + n·origin) == n·x + w. Rebasing the camera frustum this
// way lets the tests run at camera-local magnitude for Earth-scale precision
// without changing which side of the plane a point lands on. The new w is the
// difference of two large like-signed terms (w ≈ −n·lightPos, n·origin), so it
// is accumulated in DOUBLE and rounded once: at 1e6+ m a float accumulation
// leaves 0.1–0.5 m of error in w, which exceeds the sub-metre keep-bias margin
// floor and would flip a face decision for a small-range light. The stored
// plane stays float (its magnitude is now small).
inline Vector4 RebasePlane(const Vector4& p, const Vector3& origin)
{
    const double w = static_cast<double>(p.w)
                     + (static_cast<double>(p.x) * origin.x
                        + static_cast<double>(p.y) * origin.y
                        + static_cast<double>(p.z) * origin.z);
    return Vector4{p.x, p.y, p.z, static_cast<float>(w)};
}

inline Vector3 Normalize3(const Vector3& v)
{
    const float len = std::sqrt(Dot3(v, v));
    if (len <= 1e-8f)
        return Vector3{0.0f, 0.0f, 0.0f};
    const float inv = 1.0f / len;
    return Vector3{v.x * inv, v.y * inv, v.z * inv};
}

// The +axis direction and its two orthogonal tangents for each cube face. Face
// order mirrors PopulatePointShadowGeometry's kFaceDirs and GE_PointShadowFace.
struct FaceBasis
{
    Vector3 Dir;
    Vector3 TangentU;
    Vector3 TangentV;
};

const std::array<FaceBasis, kFaceCount>& FaceBases()
{
    static const std::array<FaceBasis, kFaceCount> bases = {{
        {{ 1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // +X
        {{-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // -X
        {{ 0.0f, 1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // +Y
        {{ 0.0f,-1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}}, // -Y
        {{ 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // +Z
        {{ 0.0f, 0.0f,-1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, // -Z
    }};
    return bases;
}

// A cube-face 90° perspective frustum (apex at the light) as 8 world corners +
// 6 inward planes (4 diagonal sides through the apex, near, far). For a 90° FOV
// the tangent half-extent equals the along-axis distance (tan 45° = 1).
struct FaceFrustum
{
    std::array<Vector3, 8> Corners;
    std::array<Vector4, 6> Planes;
};

FaceFrustum BuildFaceFrustum(const Vector3& lightPos, float farPlane, const FaceBasis& basis)
{
    const Vector3& d = basis.Dir;
    const Vector3& u = basis.TangentU;
    const Vector3& v = basis.TangentV;

    FaceFrustum out{};

    auto corner = [&](float distAlong, float su, float sv) {
        return Vector3{
            lightPos.x + d.x * distAlong + u.x * (su * distAlong) + v.x * (sv * distAlong),
            lightPos.y + d.y * distAlong + u.y * (su * distAlong) + v.y * (sv * distAlong),
            lightPos.z + d.z * distAlong + u.z * (su * distAlong) + v.z * (sv * distAlong)};
    };
    // Near corners collapse onto the apex (near = 0) — the pyramid spans the
    // full light reach so nothing near the light centre is excluded.
    out.Corners[0] = lightPos;
    out.Corners[1] = lightPos;
    out.Corners[2] = lightPos;
    out.Corners[3] = lightPos;
    out.Corners[4] = corner(farPlane, -1.0f, -1.0f);
    out.Corners[5] = corner(farPlane, 1.0f, -1.0f);
    out.Corners[6] = corner(farPlane, 1.0f, 1.0f);
    out.Corners[7] = corner(farPlane, -1.0f, 1.0f);

    // Region is { along-d >= |along-u|, along-d >= |along-v| }, i.e. the four
    // 45° diagonal half-spaces through the apex. Inward normals d±u, d±v.
    auto sidePlane = [&](const Vector3& sign) {
        const Vector3 n = Normalize3(Vector3{d.x + sign.x, d.y + sign.y, d.z + sign.z});
        return Vector4{n.x, n.y, n.z, -Dot3(n, lightPos)};
    };
    out.Planes[0] = sidePlane(Vector3{-u.x, -u.y, -u.z});
    out.Planes[1] = sidePlane(u);
    out.Planes[2] = sidePlane(Vector3{-v.x, -v.y, -v.z});
    out.Planes[3] = sidePlane(v);
    // Near: along-d >= 0 (apex). Far: along-d <= farPlane.
    out.Planes[4] = Vector4{d.x, d.y, d.z, -Dot3(d, lightPos)};
    out.Planes[5] = Vector4{-d.x, -d.y, -d.z, Dot3(d, lightPos) + farPlane};
    return out;
}

// Conservative disjointness over both hulls' face-normal axes only (keep-bias:
// a face-normal separation is sufficient for disjoint; missing edge-cross axes
// only ever KEEPS a truly-separated pair, never rejects an intersecting one).
bool Separated(const std::array<Vector3, 8>& cornersA, const std::array<Vector4, 6>& planesA,
               const std::array<Vector3, 8>& cornersB, const std::array<Vector4, 6>& planesB,
               float margin)
{
    for (const Vector4& p : planesA)
    {
        bool allOutside = true;
        for (const Vector3& c : cornersB)
        {
            if (PlaneDistance(p, c) >= -margin)
            {
                allOutside = false;
                break;
            }
        }
        if (allOutside)
            return true;
    }
    for (const Vector4& p : planesB)
    {
        bool allOutside = true;
        for (const Vector3& c : cornersA)
        {
            if (PlaneDistance(p, c) >= -margin)
            {
                allOutside = false;
                break;
            }
        }
        if (allOutside)
            return true;
    }
    return false;
}

} // namespace

bool PointShadowLightVisible(const Vector3& lightPositionWS, float range,
                             const PointShadowCameraCull& cam)
{
    // Camera-relative rebase (Earth-scale precision): test the range sphere in
    // camera-local space so the sphere-vs-plane distances stay small-magnitude at
    // planetary coordinates instead of losing the fp32 mantissa to big·big
    // cancellation. Translation-invariant — identical near the origin.
    const Vector3 lightRel{lightPositionWS.x - cam.CameraPosition.x,
                           lightPositionWS.y - cam.CameraPosition.y,
                           lightPositionWS.z - cam.CameraPosition.z};
    std::array<Vector4, 6> planesRel{};
    for (uint32_t i = 0; i < 6; ++i)
        planesRel[i] = RebasePlane(cam.FrustumPlanes[i], cam.CameraPosition);
    return Rendering::TestSphereFrustum(lightRel, range, planesRel.data());
}

uint8_t ComputePointShadowFaceMask(const Vector3& lightPositionWS, float range,
                                   const PointShadowCameraCull& cam)
{
    constexpr uint8_t kAllFaces = 0x3Fu;

    // Camera-relative rebase (Earth-scale precision): everything the SAT touches
    // — the light apex, the cube-face frustums built from it, and the camera
    // frustum corners/planes — is shifted by the camera position so the tests run
    // at camera-local magnitude. SAT is translation-invariant, so this is a no-op
    // near the origin and kills the fp32 cancellation at 1e6+ m coordinates.
    const Vector3 lightRel{lightPositionWS.x - cam.CameraPosition.x,
                           lightPositionWS.y - cam.CameraPosition.y,
                           lightPositionWS.z - cam.CameraPosition.z};

    // Camera inside the light's range volume: fragments immediately around the
    // camera can fall on any cube face — keep all six (the "light surrounding
    // the camera" case). In camera-local space the camera sits at the origin, so
    // lightRel is exactly the camera→light offset.
    if (Dot3(lightRel, lightRel) <= range * range)
        return kAllFaces;

    const float farPlane = std::max(range, 0.1f);
    const float margin = KeepBiasMargin(range);
    const auto& bases = FaceBases();

    std::array<Vector3, 8> cornersRel{};
    for (uint32_t i = 0; i < 8; ++i)
        cornersRel[i] = Vector3{cam.FrustumCorners[i].x - cam.CameraPosition.x,
                                cam.FrustumCorners[i].y - cam.CameraPosition.y,
                                cam.FrustumCorners[i].z - cam.CameraPosition.z};
    std::array<Vector4, 6> planesRel{};
    for (uint32_t i = 0; i < 6; ++i)
        planesRel[i] = RebasePlane(cam.FrustumPlanes[i], cam.CameraPosition);

    uint8_t mask = 0u;
    for (uint32_t face = 0; face < kFaceCount; ++face)
    {
        const FaceFrustum ff = BuildFaceFrustum(lightRel, farPlane, bases[face]);
        if (!Separated(ff.Corners, ff.Planes, cornersRel, planesRel, margin))
            mask |= static_cast<uint8_t>(1u << face);
    }
    return mask;
}

} // namespace Engine::Renderer
} // namespace GameEngine
