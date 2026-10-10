#pragma once

// CBTPlanetShading.h — the CPU-authoritative mirror of the planet surface-height field
// and its analytic normal (planet shading quality, plan §planet-shading). The sphere CBT
// used to shade with a per-triangle screen-space-derivative normal (faceted) and a base
// sphere of a single-octave sin-product relief. This header holds the SAME height math the
// GPU evaluates (cbt_domain.glsl CBT_PlanetRelief / CBT_PlanetReliefGradient and the
// cbt_surface.glsl normal), factored out so the arc-law oracles can validate it with no
// Vulkan device:
//   * the analytic relief gradient matches a central finite difference (gradient is right),
//   * the analytic shading normal matches the geometric normal of the displaced surface
//     (the whole N = normalize(dir - gradTangential(h) / (R + h)) formula is right),
//   * the normal is a function of world DIRECTION only, so it is identical from either
//     cube face at a shared edge (seam-free — the C4/C7 cross-face discipline).
//
// Lockstep contract: the functions here and their GLSL twins in cbt_domain.glsl are edited
// together, exactly like CBTSphereRoots.h <-> CBT_SphereRootCorners. The envelope of the
// relief is preserved at +/- kReliefEnvelope * amplitude regardless of octave count (the
// ampSum normalization below), so the on-shell bound in CBTSphereDomainTests stays valid.

#include <array>
#include <cmath>
#include <cstdint>

#include "CBTTerrain/CBTLayout.h"        // page constants
#include "CBTTerrain/CBTSphereFaceMap.h"  // WorldDirToFaceUV (dir -> face,uv)
#include "CBTTerrain/SphereSculptPaging.h" // SphereSculptSampler + SampleSphereSculptByDir

namespace GameEngine::CBTTerrain
{

// fBM stack shape (mirror kReliefPersistence / kReliefLacunarity in cbt_domain.glsl).
// Persistence halves the amplitude and lacunarity doubles the frequency per octave — the
// standard fBM progression. The per-octave base noise envelope is [-kReliefEnvelope,
// +kReliefEnvelope]; normalizing the octave sum by its amplitude sum keeps the TOTAL relief
// inside that same envelope for any octave count (so a deeper stack adds detail, not range).
inline constexpr float kReliefPersistence = 0.5f;
// Non-harmonic (irrational) lacunarity so octave frequencies are not integer multiples of the
// base — the axis-separable sin-product octaves at 2x/4x harmonics reinforce on a regular grid
// (a visible lattice on the sphere); 2.13 never re-aligns. Mirror of CBT_RELIEF_LACUNARITY.
inline constexpr float kReliefLacunarity = 2.13f;
inline constexpr float kReliefEnvelope = 1.5f; // max |base noise| = 1.0 + 0.5 (its two terms)

// Single-octave base noise at direction (dx,dy,dz) and angular frequency f. The two sin
// products are the pre-multi-octave CBT_PlanetRelief kernel; the envelope is [-1.5, 1.5].
inline float PlanetBaseNoise(float dx, float dy, float dz, float f)
{
    const float s1 = std::sin(dx * f);
    const float s2 = std::sin(dy * f * 1.3f + 0.7f);
    const float s3 = std::sin(dz * f * 0.7f + 1.9f);
    const float s4 = std::sin(dx * f * 2.1f + 1.3f);
    const float s5 = std::sin(dy * f * 1.7f + 2.4f);
    return s1 * s2 * s3 + 0.5f * s4 * s5;
}

// Gradient of PlanetBaseNoise w.r.t. (dx,dy,dz) — the closed-form partials of the two sin
// products (product rule). Used by the analytic surface normal.
inline std::array<float, 3> PlanetBaseNoiseGradient(float dx, float dy, float dz, float f)
{
    const float s1 = std::sin(dx * f), c1 = std::cos(dx * f);
    const float s2 = std::sin(dy * f * 1.3f + 0.7f), c2 = std::cos(dy * f * 1.3f + 0.7f);
    const float s3 = std::sin(dz * f * 0.7f + 1.9f), c3 = std::cos(dz * f * 0.7f + 1.9f);
    const float s4 = std::sin(dx * f * 2.1f + 1.3f), c4 = std::cos(dx * f * 2.1f + 1.3f);
    const float s5 = std::sin(dy * f * 1.7f + 2.4f), c5 = std::cos(dy * f * 1.7f + 2.4f);
    const float gx = f * c1 * s2 * s3 + 0.5f * (2.1f * f) * c4 * s5;
    const float gy = (1.3f * f) * s1 * c2 * s3 + 0.5f * (1.7f * f) * s4 * c5;
    const float gz = (0.7f * f) * s1 * s2 * c3;
    return {gx, gy, gz};
}

// Multi-octave (fBM) relief in metres at direction (dx,dy,dz). amplitude scales the whole
// stack; frequency is the base angular frequency (wavelength ~= 2*pi*R/frequency). octaves
// 0 is treated as 1. Normalized by the amplitude sum so the envelope is +/- kReliefEnvelope
// * amplitude for any octave count.
inline float PlanetRelief(float dx, float dy, float dz, float amplitude, float frequency,
                          uint32_t octaves)
{
    if (amplitude <= 0.0f)
        return 0.0f;
    const uint32_t oct = octaves < 1u ? 1u : octaves;
    float total = 0.0f, a = 1.0f, f = frequency, ampSum = 0.0f;
    for (uint32_t i = 0; i < oct; ++i)
    {
        total += a * PlanetBaseNoise(dx, dy, dz, f);
        ampSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    return amplitude * total / ampSum;
}

// The 3D gradient d(relief)/d(dir) of the multi-octave relief. Project out the radial
// component to get the tangential (surface) gradient used by the shading normal.
inline std::array<float, 3> PlanetReliefGradient(float dx, float dy, float dz, float amplitude,
                                                 float frequency, uint32_t octaves)
{
    if (amplitude <= 0.0f)
        return {0.0f, 0.0f, 0.0f};
    const uint32_t oct = octaves < 1u ? 1u : octaves;
    float gx = 0.0f, gy = 0.0f, gz = 0.0f, a = 1.0f, f = frequency, ampSum = 0.0f;
    for (uint32_t i = 0; i < oct; ++i)
    {
        const std::array<float, 3> g = PlanetBaseNoiseGradient(dx, dy, dz, f);
        gx += a * g[0];
        gy += a * g[1];
        gz += a * g[2];
        ampSum += a;
        a *= kReliefPersistence;
        f *= kReliefLacunarity;
    }
    const float k = amplitude / ampSum;
    return {gx * k, gy * k, gz * k};
}

// Conservative sphere occlusion (cbt_kernels.comp CBT_CornerOccluded mirror) for the horizon
// cull. Is corner P (a displaced sphere point, |P| >= r) occluded by the floor-sphere of
// radius r as seen from camera C? Exact cone test: occluded iff the angular distance between P
// and C exceeds thetaC + thetaP, cos(thetaC)=r/|C|, cos(thetaP)=r/|P|. r is the surface FLOOR
// (radius minus the max relief drop), so no visible limb terrain is ever culled — unlike a
// horizon-PLANE test (dot(P,C) < r|P|), which culls raised terrain beyond the geometric
// horizon that is still visible. Camera at/under the floor culls nothing.
inline bool SphereCornerOccluded(const std::array<float, 3>& P, const std::array<float, 3>& C,
                                 float r)
{
    const float pLen = std::sqrt(P[0] * P[0] + P[1] * P[1] + P[2] * P[2]);
    const float cLen = std::sqrt(C[0] * C[0] + C[1] * C[1] + C[2] * C[2]);
    if (pLen <= 0.0f || cLen <= r)
        return false;
    const float cosTheta = (P[0] * C[0] + P[1] * C[1] + P[2] * C[2]) / (pLen * cLen);
    const float cosC = r / cLen;
    const float sinC = std::sqrt(std::fmax(1.0f - cosC * cosC, 0.0f));
    const float cosP = std::fmin(std::fmax(r / pLen, -1.0f), 1.0f);
    const float sinP = std::sqrt(std::fmax(1.0f - cosP * cosP, 0.0f));
    return cosTheta < cosC * cosP - sinC * sinP;
}

// Refine a ray->planet hit from the analytic BASE sphere to the DISPLACED surface
// (radius + height(dir)). RaycastPlanet solves |o + t d| = radius, which ignores relief +
// sculpt, so the brush cursor drifts off tall features (#488). Fixed-point iteration closes
// the gap: at each step estimate the local surface radius R + h(dir(t)), re-solve the
// nearest ray-sphere intersection at that radius, and update t. Because the height is << R
// the per-step direction correction is small, so this converges in a few iterations. On the
// fixed point |o + t d| == radius + h(dir), i.e. the hit lies exactly on the displaced
// surface. heightFn(dx,dy,dz) returns metres above `radius` for a (not necessarily unit)
// direction. `t` is the base-sphere hit on input and the refined hit on output. `d` must be
// unit. Returns false for a non-positive radius (leaving `t` untouched).
template <typename HeightFn>
inline bool RefinePlanetHit(const std::array<float, 3>& o, const std::array<float, 3>& d,
                            float radius, HeightFn&& heightFn, float& t)
{
    if (radius <= 0.0f)
        return false;
    constexpr uint32_t kRefineIters = 6u;
    const float b = o[0] * d[0] + o[1] * d[1] + o[2] * d[2];
    const float oo = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
    for (uint32_t i = 0; i < kRefineIters; ++i)
    {
        const float px = o[0] + d[0] * t, py = o[1] + d[1] * t, pz = o[2] + d[2] * t;
        const float pLen = std::sqrt(px * px + py * py + pz * pz);
        if (pLen <= 0.0f)
            break;
        const float inv = 1.0f / pLen;
        const float r = radius + heightFn(px * inv, py * inv, pz * inv);
        // Nearest non-negative root of |o + t d|^2 = r^2.
        const float disc = b * b - (oo - r * r);
        if (disc < 0.0f)
            break; // the corrected sphere is no longer reached; keep the last t
        const float sq = std::sqrt(disc);
        float tNew = -b - sq;
        if (tNew < 0.0f)
            tNew = -b + sq; // origin inside the corrected sphere: take the far root
        if (tNew < 0.0f)
            break;
        t = tNew;
    }
    return true;
}

// The outward analytic shading normal of the relief-displaced sphere at UNIT direction
// `dir` (planet centred at the world origin, radius R). h is the total height offset above
// R (relief here; the GPU adds the sculpt sample); surfaceGrad is the TANGENTIAL gradient of
// that offset (project the 3D gradient: g - (g.dir) dir). Derivation (cbt_domain.glsl):
// the surface tangents are t*(R+h) + dir*(dh/dt), whose cross product normalizes to
// dir - surfaceGrad/(R+h). Reduces to `dir` when the surface is smooth (grad 0).
inline std::array<float, 3> SphereShadingNormalFromGrad(const std::array<float, 3>& dir, float radius,
                                                        float heightOffset,
                                                        const std::array<float, 3>& surfaceGrad)
{
    const float invScale = 1.0f / (radius + heightOffset);
    float nx = dir[0] - surfaceGrad[0] * invScale;
    float ny = dir[1] - surfaceGrad[1] * invScale;
    float nz = dir[2] - surfaceGrad[2] * invScale;
    const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {nx * inv, ny * inv, nz * inv};
}

// Convenience: the relief-only analytic normal at unit direction `dir`. Projects the relief
// gradient to the tangent plane and applies SphereShadingNormalFromGrad.
inline std::array<float, 3> PlanetShadingNormal(const std::array<float, 3>& dir, float radius,
                                                float amplitude, float frequency, uint32_t octaves)
{
    const float h = PlanetRelief(dir[0], dir[1], dir[2], amplitude, frequency, octaves);
    const std::array<float, 3> g =
        PlanetReliefGradient(dir[0], dir[1], dir[2], amplitude, frequency, octaves);
    const float gd = g[0] * dir[0] + g[1] * dir[1] + g[2] * dir[2];
    const std::array<float, 3> gs = {g[0] - gd * dir[0], g[1] - gd * dir[1], g[2] - gd * dir[2]};
    return SphereShadingNormalFromGrad(dir, radius, h, gs);
}

// ---------------------------------------------------------------------------
// Editable sculpt layer — sampled SEAM-FREE by world direction. The paged sampler
// (SampleSphereSculptByDir(const SphereSculptSampler&, ...)) lives in SphereSculptPaging.h; it
// picks the dominant cube face + face-local UV (WorldDirToFaceUV) and reads the paged bilinear.
// ---------------------------------------------------------------------------

// A unit tangent to `dir` (any consistent choice): cross with the world axis of smallest
// component magnitude, which is never parallel to dir.
inline std::array<float, 3> AnyTangent(const std::array<float, 3>& dir)
{
    const float ax = std::fabs(dir[0]), ay = std::fabs(dir[1]), az = std::fabs(dir[2]);
    std::array<float, 3> up = (ax <= ay && ax <= az) ? std::array<float, 3>{1.0f, 0.0f, 0.0f}
                              : (ay <= az)           ? std::array<float, 3>{0.0f, 1.0f, 0.0f}
                                                     : std::array<float, 3>{0.0f, 0.0f, 1.0f};
    std::array<float, 3> t = {dir[1] * up[2] - dir[2] * up[1], dir[2] * up[0] - dir[0] * up[2],
                              dir[0] * up[1] - dir[1] * up[0]};
    const float len = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    const float inv = len > 0.0f ? 1.0f / len : 0.0f;
    return {t[0] * inv, t[1] * inv, t[2] * inv};
}

// The full analytic sphere normal INCLUDING the editable sculpt layer (cbt_surface.glsl
// mirror). Relief gradient is analytic; the sculpt gradient is a central finite difference
// of the direction-sampled sculpt height along an orthonormal tangent frame (angularStep
// radians ~= one virtual texel). An invalid `sculpt` (or angularStep 0) -> relief-only.
inline std::array<float, 3> PlanetShadingNormalWithSculpt(const std::array<float, 3>& dir,
                                                          float radius, float amplitude,
                                                          float frequency, uint32_t octaves,
                                                          const SphereSculptSampler& sculpt,
                                                          float angularStep)
{
    float h = PlanetRelief(dir[0], dir[1], dir[2], amplitude, frequency, octaves);
    const std::array<float, 3> g =
        PlanetReliefGradient(dir[0], dir[1], dir[2], amplitude, frequency, octaves);
    const float gd = g[0] * dir[0] + g[1] * dir[1] + g[2] * dir[2];
    std::array<float, 3> gs = {g[0] - gd * dir[0], g[1] - gd * dir[1], g[2] - gd * dir[2]};

    if (sculpt.Valid() && angularStep > 0.0f)
    {
        const std::array<float, 3> t1 = AnyTangent(dir);
        const std::array<float, 3> t2 = {dir[1] * t1[2] - dir[2] * t1[1],
                                         dir[2] * t1[0] - dir[0] * t1[2],
                                         dir[0] * t1[1] - dir[1] * t1[0]};
        auto stepDir = [&](const std::array<float, 3>& t, float s) {
            return std::array<float, 3>{dir[0] + t[0] * s, dir[1] + t[1] * s, dir[2] + t[2] * s};
        };
        const std::array<float, 3> p1 = stepDir(t1, angularStep), m1 = stepDir(t1, -angularStep);
        const std::array<float, 3> p2 = stepDir(t2, angularStep), m2 = stepDir(t2, -angularStep);
        const float g1 = (SampleSphereSculptByDir(sculpt, p1[0], p1[1], p1[2]) -
                          SampleSphereSculptByDir(sculpt, m1[0], m1[1], m1[2])) /
                         (2.0f * angularStep);
        const float g2 = (SampleSphereSculptByDir(sculpt, p2[0], p2[1], p2[2]) -
                          SampleSphereSculptByDir(sculpt, m2[0], m2[1], m2[2])) /
                         (2.0f * angularStep);
        gs = {gs[0] + g1 * t1[0] + g2 * t2[0], gs[1] + g1 * t1[1] + g2 * t2[1],
              gs[2] + g1 * t1[2] + g2 * t2[2]};
        h += SampleSphereSculptByDir(sculpt, dir[0], dir[1], dir[2]);
    }
    return SphereShadingNormalFromGrad(dir, radius, h, gs);
}

// ---------------------------------------------------------------------------
// Radial slope + altitude material blend (cbt_surface.glsl mirror) — the sphere's principled
// replacement for the old world-Y "slope fallback". On a globe "up" is the RADIAL direction
// (dir), not world Y, so slope is measured against dir. Weights are a continuous function of
// (normal, dir, altitude) — all continuous across cube edges — so the blend is seam-free.
// ---------------------------------------------------------------------------
inline constexpr float kSlopeRockLo = 0.30f; // steepness where rock starts to appear
inline constexpr float kSlopeRockHi = 0.65f; // steepness that is fully rock
inline constexpr float kSnowLo = 0.45f;      // normalized altitude where snow starts (peaks)
inline constexpr float kSnowHi = 0.80f;      // normalized altitude that is fully snow (on flat ground)
inline constexpr float kDirtLo = -0.10f;     // grass->dirt band start (sea level ~= grass)
inline constexpr float kDirtHi = 0.40f;      // grass->dirt band end (higher ground = dirt)

inline float SmoothStep01(float lo, float hi, float x)
{
    const float t = std::fmin(std::fmax((x - lo) / (hi - lo), 0.0f), 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Layer order matches kLayerColors in cbt_surface.glsl: 0 grass, 1 rock, 2 dirt, 3 snow.
// slope01 = 1 - dot(N,dir) clamped [0,1] (steepness vs the RADIAL up); altitude is metres
// above the sphere radius R; altNorm normalizes it over the relief envelope so sea level
// (~0) reads grass, higher ground dirt, and peaks snow. Weights sum to 1 and are a
// continuous function of (normal, dir, altitude) -> seam-free across cube edges.
inline std::array<float, 4> PlanetSlopeAltitudeWeights(const std::array<float, 3>& normal,
                                                       const std::array<float, 3>& dir,
                                                       float altitude, float amplitude)
{
    const float ndotd = normal[0] * dir[0] + normal[1] * dir[1] + normal[2] * dir[2];
    const float slope01 = std::fmin(std::fmax(1.0f - ndotd, 0.0f), 1.0f);
    const float altNorm = amplitude > 0.0f
                              ? std::fmin(std::fmax(altitude / (kReliefEnvelope * amplitude), -1.0f),
                                          1.0f)
                              : 0.0f;
    const float rock = SmoothStep01(kSlopeRockLo, kSlopeRockHi, slope01);
    const float flat = 1.0f - rock;
    const float snow = flat * SmoothStep01(kSnowLo, kSnowHi, altNorm);
    const float belowSnow = flat - snow;
    const float dirtMix = SmoothStep01(kDirtLo, kDirtHi, altNorm);
    const float grass = belowSnow * (1.0f - dirtMix);
    const float dirt = belowSnow * dirtMix;
    std::array<float, 4> w = {grass, rock, dirt, snow};
    const float sum = w[0] + w[1] + w[2] + w[3];
    if (sum > 0.0f)
        for (float& v : w)
            v /= sum;
    else
        w[0] = 1.0f;
    return w;
}

// ---------------------------------------------------------------------------
// Untextured-fallback material variation
// (Includes/terrain_material_albedo.glsl CBT_ApplyMaterialVariation mirror)
// ---------------------------------------------------------------------------
// Hue-preserving value-noise variation that breaks up the flat layer tint (the "purple grass" /
// "terraced cliff band" fixes, round-8e). Kept in lockstep with the GLSL: the fragment reads this same
// math for an untextured planet layer, and the oracles below assert its two invariants headlessly —
// (1) it never shifts hue (channel ORDER of the tint is preserved: grass stays green-dominant, never
// purple), and (2) it Nyquist-fades to the flat tint as the pixel footprint grows past the noise cell.
inline float VariationFract(float x) { return x - std::floor(x); }

// hash31 (Includes/terrain_material_albedo.glsl CBT_Hash31 mirror).
inline float VariationHash31(float px, float py, float pz)
{
    px = VariationFract(px * 0.1031f);
    py = VariationFract(py * 0.1031f);
    pz = VariationFract(pz * 0.1031f);
    const float d = px * (pz + 31.32f) + py * (py + 31.32f) + pz * (px + 31.32f); // dot(p, p.zyx+31.32)
    px += d;
    py += d;
    pz += d;
    return VariationFract((px + py) * pz);
}

// Trilinear value noise (Includes/terrain_material_albedo.glsl CBT_ValueNoise mirror).
inline float VariationValueNoise(float px, float py, float pz)
{
    const float ix = std::floor(px), iy = std::floor(py), iz = std::floor(pz);
    const float fx = px - ix, fy = py - iy, fz = pz - iz;
    auto smooth = [](float f) { return f * f * (3.0f - 2.0f * f); };
    const float ux = smooth(fx), uy = smooth(fy), uz = smooth(fz);
    auto h = [&](float dx, float dy, float dz) { return VariationHash31(ix + dx, iy + dy, iz + dz); };
    auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
    const float x00 = mix(h(0, 0, 0), h(1, 0, 0), ux), x10 = mix(h(0, 1, 0), h(1, 1, 0), ux);
    const float x01 = mix(h(0, 0, 1), h(1, 0, 1), ux), x11 = mix(h(0, 1, 1), h(1, 1, 1), ux);
    return mix(mix(x00, x10, uy), mix(x01, x11, uy), uz);
}

// Fade band + lattice rotation — mirror kVariationFade{Lo,Hi} / kVariationRot in
// Includes/terrain_material_albedo.glsl.
inline constexpr float kVariationFadeLo = 0.35f;
inline constexpr float kVariationFadeHi = 1.2f;
inline std::array<float, 3> VariationRotate(float x, float y, float z)
{
    // Columns of kVariationRot (GLSL column-major mat3): out = col0*x + col1*y + col2*z.
    return {0.64864f * x - 0.57400f * y + 0.49979f * z, 0.68211f * x + 0.72972f * y - 0.04719f * z,
            -0.33762f * x + 0.37152f * y + 0.86486f * z};
}

// The composed layer color (Includes/terrain_material_albedo.glsl CBT_ApplyMaterialVariation
// mirror). base is the flat tint;
// footprintWS the world-space pixel footprint (m); scale/strength/hue the per-layer variation params.
inline std::array<float, 3> PlanetLayerVariation(const std::array<float, 3>& base, float px, float py,
                                                 float pz, float scale, float strength, float hue,
                                                 float footprintWS)
{
    if (scale <= 0.0f || (strength <= 0.0f && hue <= 0.0f))
        return base;
    const float cellsPerPixel = footprintWS * scale;
    const float t = std::fmin(std::fmax((cellsPerPixel - kVariationFadeLo) /
                                            (kVariationFadeHi - kVariationFadeLo), 0.0f), 1.0f);
    const float fade = 1.0f - t * t * (3.0f - 2.0f * t); // 1 - smoothstep
    if (fade <= 0.0f)
        return base;
    const std::array<float, 3> p = VariationRotate(px * scale, py * scale, pz * scale);
    const float n = (VariationValueNoise(p[0], p[1], p[2]) +
                     0.5f * VariationValueNoise(p[0] * 4.17f + 7.3f, p[1] * 4.17f + 7.3f,
                                                p[2] * 4.17f + 7.3f) +
                     0.25f * VariationValueNoise(p[0] * 17.4f + 19.1f, p[1] * 17.4f + 19.1f,
                                                 p[2] * 17.4f + 19.1f)) *
                    (1.0f / 1.75f);
    const float brightness = 1.0f + fade * strength * (n * 2.0f - 1.0f);
    std::array<float, 3> col = {base[0] * brightness, base[1] * brightness, base[2] * brightness};
    const float nc = VariationValueNoise(p[0] * 1.7f + 11.3f, p[1] * 1.7f + 11.3f, p[2] * 1.7f + 11.3f);
    const float sat = 1.0f + fade * hue * (nc * 2.0f - 1.0f);
    const float lum = col[0] * 0.2126f + col[1] * 0.7152f + col[2] * 0.0722f;
    return {std::fmax(lum + (col[0] - lum) * sat, 0.0f), std::fmax(lum + (col[1] - lum) * sat, 0.0f),
            std::fmax(lum + (col[2] - lum) * sat, 0.0f)};
}

} // namespace GameEngine::CBTTerrain
