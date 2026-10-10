#ifndef GE_SKY_RAY_SPHERE_GLSL
#define GE_SKY_RAY_SPHERE_GLSL

// Ray/sphere intersection at planet scale, in the two parameterisations the sky passes use:
// a world-space ray (vec3 origin + direction) and the transmittance LUT's (radius, zenith
// cosine) pair.
//
// The trap every form here is written around: at planet scale r*r and sphereRadius*sphereRadius
// are both ~4e13, so their difference is a catastrophic float32 cancellation — and the compiler
// is free to contract one of the two products into an FMA (nothing marks these NoContraction),
// which leaves the other one rounded and the difference carrying a residual of ~1e6 m^2 where
// the exact answer is 0. A grazing solve that should return a hundreds-of-kilometres chord then
// returns a root of a few metres. Two rules follow, and both are load-bearing:
//   - never subtract the two squares directly: factor as (a - b) * (a + b), which is exact for
//     same-magnitude operands and gives contraction nothing to fuse;
//   - take each root in its non-cancelling direction (c / tFar instead of -q - sqrt(disc)).

bool GE_RaySphereIntersect(vec3 r0, vec3 rd, vec3 s0, float sR, out float t0, out float t1)
{
    vec3 L = s0 - r0;
    float tca = dot(L, rd);
    // Perpendicular-distance form: the off-axis component of L gives the squared miss distance
    // directly, without the dot(L, L) - tca*tca cancellation (both terms are ~4e13 at planet
    // scale, so their difference keeps no precision at all).
    vec3 perp = L - tca * rd;
    float d2 = dot(perp, perp);
    if (d2 > sR * sR) return false;
    float thc = sqrt(max(sR * sR - d2, 0.0));
    t0 = tca - thc;
    t1 = tca + thc;
    return true;
}

// Distance from radius r along zenith cosine mu to a sphere that ENCLOSES the origin
// (r <= sphereRadius): the single forward exit of the shell. Returns 0.0 only on the boundary
// looking outward, where there is no shell left to cross.
float GE_DistanceToOuterSphere(float r, float mu, float sphereRadius)
{
    float shell = (sphereRadius - r) * (sphereRadius + r); // sphereRadius^2 - r^2, without the cancellation
    float rMu = r * mu;
    float q = sqrt(max(shell + rMu * rMu, 0.0));
    // Roots of t^2 + 2*rMu*t - shell; the forward one is q - rMu. For an outward ray (rMu > 0)
    // that subtracts near-equal terms, so use the equivalent product form instead.
    return (rMu > 0.0) ? shell / (q + rMu) : q - rMu;
}

// Nearest forward distance from radius r along zenith cosine mu to a sphere the origin is
// OUTSIDE or exactly ON (r >= sphereRadius). Returns 0.0 when the ray misses it or meets it
// only behind the origin. Exactly on the surface both roots collapse to the chord -2*r*mu,
// which is what an origin sitting on the ground gets.
float GE_DistanceToInnerSphere(float r, float mu, float sphereRadius)
{
    float rMu = r * mu;
    float gap = (r - sphereRadius) * (r + sphereRadius); // r^2 - sphereRadius^2, without the cancellation
    float disc = rMu * rMu - gap;
    if (disc < 0.0)
        return 0.0;
    float tFar = -rMu + sqrt(disc);
    if (tFar <= 0.0)
        return 0.0;
    float tNear = gap / tFar; // product of the roots over the far one: no cancelling subtraction
    return (tNear > 0.0) ? tNear : tFar;
}

#endif // GE_SKY_RAY_SPHERE_GLSL
