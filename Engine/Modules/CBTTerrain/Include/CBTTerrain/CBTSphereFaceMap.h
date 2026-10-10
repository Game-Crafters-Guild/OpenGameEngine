#pragma once

// CBTSphereFaceMap.h — the (face, rect) region-identity foundation for editing a
// spherical CBT terrain (planet). The C7 cube-sphere renders the globe but had no
// editable height source: relief was a pure procedural function of the surface
// direction (cbt_domain.glsl CBT_PlanetRelief). To EDIT the planet we need a stable
// way to name "where on the sphere" an edit lands and to store it — the planar
// analogue of a UV rect on a single heightmap.
//
// Identity: a point on the sphere maps to exactly one CUBE FACE (the six faces of the
// C7 cube-sphere, CBTSphereRoots.h) plus a FACE-LOCAL UV in [0,1]^2. An edit is then a
// set of (faceIndex, face-local UV rect) regions — one for an interior edit, two when
// the brush straddles a cube edge, three at a cube corner. This mirrors the planar
// DirtyRegionLog rect but carries the face, so C5 region reclassification and the bake
// can run per face on the sphere just as they do on one planar heightmap.
//
// Exactness (matches the C7 precision argument, cbt_domain.glsl): the face table and
// the corner decode are the SAME kCubeFaces the GPU decodes geometry from, so the CPU
// face-map and the shader agree by construction. A point exactly on a cube edge/corner
// has integer cube coordinates, so its face-local UV lands exactly on {0, 0.5, 1} and
// the round-trip (dir -> (face,uv) -> dir) is exact there; interior points round-trip
// to within fp32 (the projection is a division by the dominant component, which cancels
// under the inverse). The GLSL mirror is CBT_FaceUVFromCube / CBT_FaceOfRoot in
// cbt_domain.glsl — edited in lockstep, like CBTSphereRoots.h <-> CBT_SphereRootCorners.
//
// Storage layout (the "single wrapped source" design decision, plan §planet-editing):
// the six faces' sculpt payloads are packed into ONE vertically-stacked atlas (width W,
// height 6*H): face f owns rows [f*H, (f+1)*H). The atlas lives in its OWN host-visible
// SSBO ring (binding 16, kSphereSculptBinding) — the smallest correct GPU step given the
// sphere samples it additively (per-band clamped bilinear, no cross-band bleed), written
// once per frame like the params UBO. Per-face rects map to atlas texel bands; a cross-
// face edit produces regions in two (or three) bands.

#include <array>
#include <cmath>
#include <cstdint>

#include "CBTTerrain/CBTSphereRoots.h" // kCubeFaces, kCubeFaceCount, CBTCubeCorner

namespace GameEngine::CBTTerrain
{

// A face-local coordinate on the cube-sphere: one of the six faces + UV in [0,1]^2.
struct SphereFaceUV
{
    uint32_t Face = 0u;
    float U = 0.0f;
    float V = 0.0f;
};

// A rectangular sub-region of one face, in face-local UV [0,1]. Half-open in the sense
// that Max is the far edge; IsEmpty when degenerate. This is the sphere analogue of
// DirtyRegionLog::Region (which is planar texel space).
struct SphereFaceUVRect
{
    uint32_t Face = 0u;
    float MinU = 0.0f;
    float MinV = 0.0f;
    float MaxU = 0.0f;
    float MaxV = 0.0f;

    bool IsEmpty() const { return MaxU <= MinU || MaxV <= MinV; }
};

// Up to six face regions from one edit (an interior edit is 1, an edge straddle 2, a
// corner 3; a pathologically large cap could touch more, bounded by the six faces).
struct SphereEditRegions
{
    std::array<SphereFaceUVRect, kCubeFaceCount> Rects{};
    uint32_t Count = 0u;
};

namespace Detail
{
// Float cube corner (components in {-1,+1}) for kCubeFaces index `cornerIdx`.
inline std::array<float, 3> CubeCornerF(int cornerIdx)
{
    return {(cornerIdx & 1) ? 1.0f : -1.0f, (cornerIdx & 2) ? 1.0f : -1.0f,
            (cornerIdx & 4) ? 1.0f : -1.0f};
}

// The four CCW-from-outside corners of face `f`, as float vec3, from the SAME face
// table the GPU decodes geometry from (kCubeFaces). Q0 is the UV origin, (Q1-Q0) the
// U axis, (Q3-Q0) the V axis — an axis-aligned square, so the parametrization is exact
// affine (u,v in [0,1] on the face).
inline void FaceCorners(uint32_t f, std::array<float, 3>& q0, std::array<float, 3>& q1,
                        std::array<float, 3>& q3)
{
    q0 = CubeCornerF(kCubeFaces[f * 4u + 0u]);
    q1 = CubeCornerF(kCubeFaces[f * 4u + 1u]);
    q3 = CubeCornerF(kCubeFaces[f * 4u + 3u]);
}

// A cube-plane point `p` (already on face f's plane, i.e. p[axis] == +/-1) -> face-local
// UV via the exact affine face parametrization. Shared by the dominant-face map and the
// project-onto-any-face classifier so both use one convention.
inline void CubePosToFaceUV(uint32_t f, const float p[3], float& u, float& v)
{
    std::array<float, 3> q0, q1, q3;
    FaceCorners(f, q0, q1, q3);
    const float du[3] = {q1[0] - q0[0], q1[1] - q0[1], q1[2] - q0[2]};
    const float dv[3] = {q3[0] - q0[0], q3[1] - q0[1], q3[2] - q0[2]};
    const float rel[3] = {p[0] - q0[0], p[1] - q0[1], p[2] - q0[2]};
    const float uu = du[0] * du[0] + du[1] * du[1] + du[2] * du[2];
    const float vv = dv[0] * dv[0] + dv[1] * dv[1] + dv[2] * dv[2];
    u = uu > 0.0f ? (rel[0] * du[0] + rel[1] * du[1] + rel[2] * du[2]) / uu : 0.0f;
    v = vv > 0.0f ? (rel[0] * dv[0] + rel[1] * dv[1] + rel[2] * dv[2]) / vv : 0.0f;
}
} // namespace Detail

// Project `dir` onto cube face `f` along that face's axis. Returns false when the
// direction is on the wrong side of the axis or would project outside the face square
// (beyond [0,1]^2 by more than `eps`). Near a cube edge a direction projects in-range
// on BOTH adjacent faces (and on three at a corner) — this is what lets a cross-face
// edit reach the shared boundary from both faces (no gap at the edge). face = axis*2 +
// (positive ? 0 : 1). MUST match WorldDirToFaceUV's face convention.
inline bool ProjectDirOntoFace(uint32_t f, float dx, float dy, float dz, float eps, float& u,
                               float& v)
{
    const uint32_t axis = f >> 1u;
    const bool positive = (f & 1u) == 0u;
    const float comp = (axis == 0u) ? dx : (axis == 1u) ? dy : dz;
    if (positive ? (comp <= 0.0f) : (comp >= 0.0f))
        return false;
    const float denom = std::fabs(comp);
    if (denom <= 0.0f)
        return false;
    const float inv = 1.0f / denom;
    const float p[3] = {dx * inv, dy * inv, dz * inv};
    Detail::CubePosToFaceUV(f, p, u, v);
    return u >= -eps && u <= 1.0f + eps && v >= -eps && v <= 1.0f + eps;
}

// dir (need NOT be unit length; the planet is centred at the world origin) -> the cube
// face it projects onto + the face-local UV. The dominant-magnitude axis picks the
// face; dividing by that component puts the point on the cube face plane; the affine
// face parametrization yields UV. Exact on cube edges/corners (integer cube coords).
inline SphereFaceUV WorldDirToFaceUV(float x, float y, float z)
{
    const float ax = std::fabs(x);
    const float ay = std::fabs(y);
    const float az = std::fabs(z);

    uint32_t axis;
    float sign;
    if (ax >= ay && ax >= az)
    {
        axis = 0u;
        sign = x >= 0.0f ? 1.0f : -1.0f;
    }
    else if (ay >= az)
    {
        axis = 1u;
        sign = y >= 0.0f ? 1.0f : -1.0f;
    }
    else
    {
        axis = 2u;
        sign = z >= 0.0f ? 1.0f : -1.0f;
    }
    // face = axis*2 + (positive ? 0 : 1). Matches kCubeFaces: 0=+X,1=-X,2=+Y,3=-Y,4=+Z,5=-Z.
    const uint32_t face = axis * 2u + (sign > 0.0f ? 0u : 1u);

    // cubePos = dir / |dir[axis]| -> the dominant component is exactly +/-1.
    const float denom = (axis == 0u) ? ax : (axis == 1u) ? ay : az;
    const float inv = denom > 0.0f ? 1.0f / denom : 0.0f;
    const float p[3] = {x * inv, y * inv, z * inv};

    SphereFaceUV out{};
    out.Face = face;
    Detail::CubePosToFaceUV(face, p, out.U, out.V);
    return out;
}

// (face, faceUV) -> unit direction (the inverse of WorldDirToFaceUV). P on the face
// plane = Q0 + u*(Q1-Q0) + v*(Q3-Q0); normalize to the sphere. Round-trips exactly on
// corners/edges, to fp32 in the interior.
inline void FaceUVToWorldDir(uint32_t face, float u, float v, float& x, float& y, float& z)
{
    std::array<float, 3> q0, q1, q3;
    Detail::FaceCorners(face, q0, q1, q3);
    const float px = q0[0] + u * (q1[0] - q0[0]) + v * (q3[0] - q0[0]);
    const float py = q0[1] + u * (q1[1] - q0[1]) + v * (q3[1] - q0[1]);
    const float pz = q0[2] + u * (q1[2] - q0[2]) + v * (q3[2] - q0[2]);
    const float len = std::sqrt(px * px + py * py + pz * pz);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    x = px * inv;
    y = py * inv;
    z = pz * inv;
}

// Classify a spherical-cap edit (centre direction + angular radius, radians) into per-
// face UV rects. The cap boundary is sampled as a ring of directions plus the centre;
// each sample is classified to (face, uv) and unioned into that face's rect. A cap that
// straddles a cube edge lands samples on both faces (near the shared boundary), so both
// faces get a rect that reaches the boundary — the union covers the cap with no gap at
// the edge (the C4 T-junction lesson: gaps are invisible to area tests, so the oracle
// samples the edge explicitly). Any rect edge within `snapEps` of 0/1 snaps to it so a
// straddling edit's two rects meet exactly at the shared cube edge.
inline SphereEditRegions ClassifySphereCapEdit(float cx, float cy, float cz, float angularRadius)
{
    SphereEditRegions out{};
    // Normalize the centre direction.
    const float clen = std::sqrt(cx * cx + cy * cy + cz * cz);
    if (clen <= 0.0f)
        return out;
    const float nx = cx / clen, ny = cy / clen, nz = cz / clen;

    // Build a tangent frame (t, b) about the centre direction n.
    const float upx = std::fabs(ny) < 0.99f ? 0.0f : 1.0f;
    const float upy = std::fabs(ny) < 0.99f ? 1.0f : 0.0f;
    const float upz = 0.0f;
    // t = normalize(up x n)
    float tx = upy * nz - upz * ny;
    float ty = upz * nx - upx * nz;
    float tz = upx * ny - upy * nx;
    const float tl = std::sqrt(tx * tx + ty * ty + tz * tz);
    const float tinv = tl > 0.0f ? 1.0f / tl : 0.0f;
    tx *= tinv;
    ty *= tinv;
    tz *= tinv;
    // b = n x t
    const float bx = ny * tz - nz * ty;
    const float by = nz * tx - nx * tz;
    const float bz = nx * ty - ny * tx;

    // Per-face accumulators.
    std::array<bool, kCubeFaceCount> touched{};
    std::array<float, kCubeFaceCount> minU{}, minV{}, maxU{}, maxV{};
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
    {
        minU[f] = minV[f] = 1e30f;
        maxU[f] = maxV[f] = -1e30f;
    }
    // Accumulate a sample into EVERY face it projects in-range onto (not just the
    // dominant one): a sample near a cube edge lands on both adjacent faces, so both
    // faces' rects reach the shared boundary and the union has no gap at the edge.
    constexpr float kProjEps = 1e-4f;
    auto accumulate = [&](float dx, float dy, float dz) {
        for (uint32_t f = 0; f < kCubeFaceCount; ++f)
        {
            float u, v;
            if (!ProjectDirOntoFace(f, dx, dy, dz, kProjEps, u, v))
                continue;
            u = u < 0.0f ? 0.0f : (u > 1.0f ? 1.0f : u);
            v = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
            touched[f] = true;
            minU[f] = std::fmin(minU[f], u);
            minV[f] = std::fmin(minV[f], v);
            maxU[f] = std::fmax(maxU[f], u);
            maxV[f] = std::fmax(maxV[f], v);
        }
    };

    accumulate(nx, ny, nz); // centre
    // Two rings (boundary + mid) so a cap that clips a face corner still lands enough
    // samples on the far face to bound its rect. 24 samples/ring is ample for brush caps.
    constexpr uint32_t kRingSamples = 24u;
    const float kTwoPi = 6.28318530717958647692f;
    for (uint32_t ring = 0; ring < 2u; ++ring)
    {
        const float r = angularRadius * (ring == 0u ? 1.0f : 0.5f);
        const float cr = std::cos(r);
        const float sr = std::sin(r);
        for (uint32_t i = 0; i < kRingSamples; ++i)
        {
            const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(kRingSamples);
            const float ct = std::cos(a);
            const float st = std::sin(a);
            // dir = n*cos(r) + (t*cos(a) + b*sin(a))*sin(r)
            const float dx = nx * cr + (tx * ct + bx * st) * sr;
            const float dy = ny * cr + (ty * ct + by * st) * sr;
            const float dz = nz * cr + (tz * ct + bz * st) * sr;
            accumulate(dx, dy, dz);
        }
    }

    constexpr float kSnapEps = 1e-4f;
    for (uint32_t f = 0; f < kCubeFaceCount; ++f)
    {
        if (!touched[f])
            continue;
        float lo0 = minU[f], lo1 = minV[f], hi0 = maxU[f], hi1 = maxV[f];
        if (lo0 < kSnapEps) lo0 = 0.0f;
        if (lo1 < kSnapEps) lo1 = 0.0f;
        if (hi0 > 1.0f - kSnapEps) hi0 = 1.0f;
        if (hi1 > 1.0f - kSnapEps) hi1 = 1.0f;
        SphereFaceUVRect& rect = out.Rects[out.Count++];
        rect.Face = f;
        rect.MinU = lo0;
        rect.MinV = lo1;
        rect.MaxU = hi0;
        rect.MaxV = hi1;
    }
    return out;
}

} // namespace GameEngine::CBTTerrain
