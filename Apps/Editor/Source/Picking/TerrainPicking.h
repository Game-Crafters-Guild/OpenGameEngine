#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "CBTTerrain/CBTPlanetShading.h" // RefinePlanetHit (analytic hit -> displaced surface)
#include "ECS/ECS.h"
#include "Mathematics/Ray.h"
#include "Mathematics/Vector3.h"
#include "Types/Types.h"

namespace GameEngine::ECS { class World; }

namespace GameEngine::Editor::Picking
{

struct TerrainPickHit
{
    GameEngine::ECS::EntityHandle Entity{};
    Mathematics::Vector3          WorldPosition{};
    Mathematics::Vector3          WorldNormal{};
    float32                       Distance = 0.0f;
};

// Coarse samples marched along the ray's traversal of a terrain's world AABB, then a fixed
// bisection refinement. The coarse step scales to the traversed extent, so the marcher's
// reach is the whole terrain regardless of size (a 4 km terrain no longer clips at 1 km);
// cost stays bounded at kTerrainMarchSteps + 1 coarse samples (both endpoints of the
// traversed span are sampled) plus kTerrainMarchRefineIters refinement samples per terrain
// per pick. Refinement is 8 halvings so even a large-terrain coarse step lands sub-
// decimetre (e.g. a 10 m coarse step -> ~4 cm).
inline constexpr uint32 kTerrainMarchSteps = 512u;
inline constexpr uint32 kTerrainMarchRefineIters = 8u;

// Ray-marches a terrain heightfield within its world AABB and returns the nearest surface
// crossing (ray parameter `outT`, world position `outPos`). The ray is clipped to the AABB
// slab so only the traversed span is sampled — a ray that misses the box returns false
// early (a free broadphase reject). sampleHeight(wx, wz, outY) returns the world-space
// surface height at (wx,wz), or false outside the footprint / when the containing tile is
// not resident. `tMax` caps the search (e.g. a closer terrain's hit). Direction need not be
// unit; `outT` is in the ray's own parameter units, consistent with `tMax`.
template <typename SampleHeightFn>
inline bool MarchTerrainSurface(const Mathematics::Ray3D& ray,
                                const Mathematics::Vector3& aabbMin,
                                const Mathematics::Vector3& aabbMax,
                                float32 tMax, SampleHeightFn&& sampleHeight,
                                float32& outT, Mathematics::Vector3& outPos)
{
    const float32 o[3] = {ray.origin.x, ray.origin.y, ray.origin.z};
    const float32 d[3] = {ray.direction.x, ray.direction.y, ray.direction.z};
    const float32 lo[3] = {aabbMin.x, aabbMin.y, aabbMin.z};
    const float32 hi[3] = {aabbMax.x, aabbMax.y, aabbMax.z};

    // Ray-AABB slab clip -> [tEnter, tExit], clamped to [0, tMax].
    constexpr float32 kParallelEps = 1e-8f;
    float32 tEnter = 0.0f;
    float32 tExit = tMax;
    for (int a = 0; a < 3; ++a)
    {
        if (std::abs(d[a]) < kParallelEps)
        {
            if (o[a] < lo[a] || o[a] > hi[a])
                return false; // parallel to this slab and outside it: no intersection
            continue;
        }
        float32 t0 = (lo[a] - o[a]) / d[a];
        float32 t1 = (hi[a] - o[a]) / d[a];
        if (t0 > t1)
            std::swap(t0, t1);
        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
    }
    if (tEnter > tExit)
        return false; // ray misses the terrain's bounding box entirely

    const float32 span = tExit - tEnter;
    const float32 stepSize = span / static_cast<float32>(kTerrainMarchSteps);

    auto pointAt = [&](float32 t) {
        return Mathematics::Vector3(o[0] + d[0] * t, o[1] + d[1] * t, o[2] + d[2] * t);
    };

    // Coarse march: first sample whose ray height is at/below the surface brackets the hit.
    for (uint32 i = 0; i <= kTerrainMarchSteps; ++i)
    {
        const float32 t = tEnter + stepSize * static_cast<float32>(i);
        if (t > tExit)
            break;

        const Mathematics::Vector3 p = pointAt(t);
        float32 surfY;
        if (!sampleHeight(p.x, p.z, surfY))
            continue; // outside the footprint / non-resident tile: not a crossing here
        if (p.y > surfY)
            continue; // above the surface: keep marching

        // Crossed at or before `t`. Bracket [loT, hiT] with loT above, hiT at/below.
        float32 loT = (i > 0) ? (tEnter + stepSize * static_cast<float32>(i - 1)) : tEnter;
        float32 hiT = t;
        for (uint32 r = 0; r < kTerrainMarchRefineIters; ++r)
        {
            const float32 mid = (loT + hiT) * 0.5f;
            const Mathematics::Vector3 m = pointAt(mid);
            float32 mh;
            if (!sampleHeight(m.x, m.z, mh))
            {
                hiT = mid; // treat an out-of-footprint sample as above the surface
                continue;
            }
            if (m.y > mh)
                loT = mid;
            else
                hiT = mid;
        }
        outT = (loT + hiT) * 0.5f;
        outPos = pointAt(outT);
        return true;
    }
    return false;
}

// Coarse samples along the chord a ray traverses INSIDE the reference sphere, plus the
// bisection that closes the bracket. The chord is at most 2 * radius long, so on the 2 km test
// planet the coarse step is under 8 m against the ~100 m arc wavelength of the relief's
// fastest fBM octave (frequency * lacunarity^3 at the shipped four octaves) — roughly thirteen
// samples across the smallest procedural feature. A sculpt dab narrower than the step can be
// stepped over; 16 halvings place a bracketed crossing within a tenth of a millimetre.
inline constexpr uint32 kPlanetChordMarchSteps = 512u;
inline constexpr uint32 kPlanetChordRefineIters = 16u;

// First crossing of the DISPLACED planet surface along [0, tEnd]. The signed gap
// |p(t)| - (radius + height(p(t))) is positive above the ground and negative below it, so the
// first non-positive coarse sample brackets the crossing and bisection closes it. `d` must be
// unit and `outT` is in unit-direction units. False when the whole span stays above the
// ground, and false when the origin is already below it (there is no surface ahead of a ray
// that starts underground).
template <typename HeightFn>
inline bool MarchPlanetChordSurface(const Mathematics::Vector3& o, const Mathematics::Vector3& d,
                                    float32 radius, float32 tEnd, HeightFn& heightAboveRadius,
                                    float32& outT)
{
    if (!(tEnd > 0.0f))
        return false;

    auto surfaceGap = [&](float32 t) {
        const float32 px = o.x + d.x * t;
        const float32 py = o.y + d.y * t;
        const float32 pz = o.z + d.z * t;
        const float32 len = std::sqrt(px * px + py * py + pz * pz);
        return len - (radius + heightAboveRadius(px, py, pz));
    };

    if (surfaceGap(0.0f) <= 0.0f)
        return false;

    const float32 stepSize = tEnd / static_cast<float32>(kPlanetChordMarchSteps);
    float32 loT = 0.0f;
    for (uint32 i = 1; i <= kPlanetChordMarchSteps; ++i)
    {
        const float32 t = stepSize * static_cast<float32>(i);
        if (surfaceGap(t) > 0.0f)
        {
            loT = t; // still above the surface: keep marching
            continue;
        }

        float32 hiT = t;
        for (uint32 r = 0; r < kPlanetChordRefineIters; ++r)
        {
            const float32 mid = (loT + hiT) * 0.5f;
            if (surfaceGap(mid) > 0.0f)
                loT = mid;
            else
                hiT = mid;
        }
        outT = (loT + hiT) * 0.5f;
        return true;
    }
    return false;
}

// Ray -> spherical (planet) terrain surface. The planet is a sphere of `radius` centred at
// the WORLD origin: cbt_kernels.comp builds every sphere vertex as dir * (radius + height)
// with no entity offset, so the entity's transform does not move it. `heightAboveRadius(dx,
// dy, dz)` returns metres above `radius` at a (not necessarily unit) world direction. That
// height is SIGNED — the drawn ground dips into valleys below the reference sphere as readily
// as it rises into peaks above it — which is what splits the solve in two:
//
//   * Origin OUTSIDE the reference sphere: the analytic near root — the entry point on the
//     REFERENCE sphere, which ignores relief — seeds RefinePlanetHit, a six-iteration fixed
//     point that at each step evaluates the height at its CURRENT point and re-solves the near
//     root of the sphere of radius + that height. Away from grazing incidence, wherever that
//     corrected sphere stays reachable, the iteration converges onto the displaced surface. It
//     fails two ways:
//       - Over ground that dips BELOW the reference radius the corrected sphere is the smaller
//         one, and a shallow ray misses it: the discriminant goes negative on the first
//         iteration, the loop breaks, and the seed is returned unchanged — a hit ON the
//         reference sphere, short of the drawn ground.
//       - At GRAZING incidence the fixed point does not converge within the six iterations: no
//         discriminant goes negative, and the loop stops wherever its last step lands — tens of
//         metres off the reference sphere and the drawn surface alike.
//   * Origin INSIDE it — any camera below the reference radius, which is the ordinary pose for
//     a camera near the surface over a valley — the analytic solve has NO near root: its only
//     non-negative root is where the ray leaves the sphere on the far side of the planet.
//     Seeding a refinement there answers with the opposite hemisphere. The ground in front of
//     the camera is found by marching the interior chord for the first surface crossing.
//
// Measured (TerrainPickingRayTests) against a dense march of the drawn surface:
//   * Discriminant break: a camera 0.5 m above the reference sphere over a 56.11 m valley,
//     looking 10 degrees down, reports a hit 2.89 m out, on the reference sphere — the drawn
//     ground is 339.60 m ahead, so the pick is 336.71 m short. The failure is a wrong hit, not
//     a miss: nothing downstream can tell it apart from a good one.
//   * Non-convergence: over a 32-azimuth sweep of the visible disc from a 1.5 R orbit the worst
//     positional error is 175.81 m, nearly twice the relief envelope, at 94.9% of the
//     silhouette — a ray that runs all six iterations and stops off both spheres.
//   * No seed: a near-limb ray that clears the reference sphere but would graze a relief peak
//     has no analytic root at all and reports nothing: 33 of 3937 drawn rays in that sweep.
// The inside branch has a miss of its own: the chord march stops where the ray leaves the
// reference sphere, so ground standing above that radius further along the ray is not found.
//
// `tMax` caps the search (e.g. a closer mesh's hit). Direction need not be unit; `outT` is in
// the ray's own parameter units, consistent with `tMax`. `outNormal` is the outward radial
// direction at the hit.
template <typename HeightFn>
inline bool SolvePlanetSurfaceHit(const Mathematics::Ray3D& ray, float32 radius, float32 tMax,
                                  HeightFn&& heightAboveRadius, float32& outT,
                                  Mathematics::Vector3& outPos, Mathematics::Vector3& outNormal)
{
    if (!(radius > 0.0f))
        return false;
    const float32 dirLen = ray.direction.Length();
    if (!(dirLen > 0.0f))
        return false;

    const Mathematics::Vector3 o = ray.origin;
    const Mathematics::Vector3 d = ray.direction.Normalize();
    const float32 b = Mathematics::Vector3::Dot(o, d);
    const float32 c = Mathematics::Vector3::Dot(o, o) - radius * radius;
    const float32 disc = b * b - c;
    if (disc < 0.0f)
        return false;
    const float32 sq = std::sqrt(disc);

    float32 tUnit = 0.0f;
    if (c > 0.0f)
    {
        // Outside the reference sphere. A negative near root here means the whole sphere is
        // behind the origin (sq < |b| when c > 0, so the far root is negative too).
        tUnit = -b - sq;
        if (tUnit < 0.0f)
            return false;
        CBTTerrain::RefinePlanetHit(std::array<float32, 3>{o.x, o.y, o.z},
                                    std::array<float32, 3>{d.x, d.y, d.z}, radius,
                                    heightAboveRadius, tUnit);
    }
    else
    {
        // Inside (or on) the reference sphere: march the chord up to where the ray leaves it,
        // capped by the caller's reach so the walk is never longer than the useful range.
        const float32 tExit = -b + sq;
        const float32 tEnd = std::min(tExit, tMax * dirLen);
        if (!MarchPlanetChordSurface(o, d, radius, tEnd, heightAboveRadius, tUnit))
            return false;
    }

    // The solve ran on a unit direction; report t in the caller's ray units so it is
    // comparable with `tMax` and with the planar march's t.
    const float32 t = tUnit / dirLen;
    if (t < 0.0f || t >= tMax)
        return false;

    outT = t;
    outPos = Mathematics::Vector3(o.x + d.x * tUnit, o.y + d.y * tUnit, o.z + d.z * tUnit);
    outNormal = outPos.Normalize();
    return true;
}

// Cast a ray against the active terrain in the world and return the closest hit, refined to
// the surface. Planar terrains are marched over their heightfield footprint; the spherical
// (planet) domain is solved against the displaced sphere the CBT renderer draws. A spherical
// terrain is never tested against its residual planar heightfield — that footprint is not
// drawn, so presenting it as a pick target would be a phantom. `maxDistance` bounds the ray
// length; hits beyond it are not reported.
bool RaycastTerrain(const Mathematics::Ray3D& ray,
                    GameEngine::ECS::World& world,
                    float32 maxDistance,
                    TerrainPickHit& outHit);

}
