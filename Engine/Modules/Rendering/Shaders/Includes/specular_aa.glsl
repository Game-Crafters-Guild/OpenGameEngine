#ifndef GE_SPECULAR_AA_GLSL
#define GE_SPECULAR_AA_GLSL

#include "compat_profile.glsl"

// Geometric specular anti-aliasing.
//
// Estimates the screen-space variance of the shading normal (the rate it changes
// across the pixel) and clamps the material roughness UP by it, widening the GGX
// lobe to cover the sub-pixel normal variation. This suppresses the specular
// shimmer/sparkle that crawls on detailed normal maps, dense curved meshes, and
// sharp highlights — the inside-triangle SHADING aliasing that MSAA does not fix
// (screen-space derivatives are per-2x2-quad, independent of sample count).
//
// Energy-derived: the variance is added in alpha^2 (squared-GGX) space and folded
// back to perceptual roughness, applied as a max() floor so it only ever widens
// the lobe, never sharpens. Drives the single so.roughness field, so it reaches
// the direct GGX lobe, the IBL prefilter LOD, spec-AO, and the anisotropy alpha
// split together (all read the field downstream); the coat roughness is floored
// alongside since the coat reuses the base shading normal.
//
// MUST be called after so.normalWS is finalized (post-normalize, post two-sided
// flip) and before any lobe or IBL-LOD read of so.roughness:  GE_ApplySpecularAA(so);

// Screen-space variance scale on the squared normal-derivative length. Raise toward
// 1.0 if shimmer persists on very high-frequency normal maps; lower if intended
// gloss reads too soft head-on.
const float kSpecularAAVariance  = 0.5;
// Hard clamp on the added variance — bounds the widening on silhouettes and
// normal-map seams where the normal derivative spikes (prevents a matte halo).
const float kSpecularAAThreshold = 0.18;

// Widen perceptualRoughness to cover an added NDF variance (in alpha^2 space) and
// return it as a floor — the result is always >= the input.
float GE_SpecularAAFloor(float perceptualRoughness, float kernelRoughness2)
{
    float alpha         = perceptualRoughness * perceptualRoughness;
    float filteredAlpha2 = clamp(alpha * alpha + kernelRoughness2, 0.0, 1.0);
    return max(perceptualRoughness, sqrt(sqrt(filteredAlpha2)));
}

// Screen-space NDF-filter kernel (alpha² space) from a shading normal's per-pixel derivatives.
// The normal is unit length, so dot(dN, dN) is the isotropic variance estimate. The 2.0 is the
// Tokuyoshi isotropic NDF-filter factor; with kSpecularAAVariance it nets ~1x the raw derivative
// sum (not a double-count). Threshold hard-clamps it on silhouettes / normal-map seams.
float GE_NormalVarianceKernel(vec3 nrm)
{
    vec3  dNdx     = GE_DFDX(nrm);
    vec3  dNdy     = GE_DFDY(nrm);
    float variance = kSpecularAAVariance * (dot(dNdx, dNdx) + dot(dNdy, dNdy));
    return min(2.0 * variance, kSpecularAAThreshold);
}

void GE_ApplySpecularAA(inout SurfaceOutput so)
{
    float kernelRoughness2 = GE_NormalVarianceKernel(so.normalWS);

    so.roughness = GE_SpecularAAFloor(so.roughness, kernelRoughness2);

#ifdef GE_CLEARCOAT_ENABLED
  #ifdef GE_COAT_NORMAL_ENABLED
    // The coat shades about its OWN normal, so estimate the coat normal's screen-space
    // variance independently — a high-frequency coat normal map shimmers on its own axis,
    // unrelated to the base normal's derivatives.
    so.clearCoatRoughness = GE_SpecularAAFloor(so.clearCoatRoughness, GE_NormalVarianceKernel(so.coatNormalWS));
  #else
    // The coat lobe shades about the base normal, so the same variance applies to its
    // independent roughness.
    so.clearCoatRoughness = GE_SpecularAAFloor(so.clearCoatRoughness, kernelRoughness2);
  #endif
#endif
}

#ifdef GE_CLEARCOAT_ENABLED
// Coat roughening: a rough clear coat scatters the base reflection on the way in and out,
// so widen the base roughness by the coat's NDF variance (coat alpha², in the same alpha²
// space GE_SpecularAAFloor works in), weighted by coat presence, as a max() floor — it only
// ever blurs the base, never sharpens. A smooth coat (clearCoatRoughness ~0) or coat == 0 is
// an exact no-op. The coat's own lobe keeps its sharp clearCoatRoughness downstream. Raising
// the single so.roughness field reaches the base GGX lobe, the IBL prefilter LOD, the BRDF
// LUT, spec-AO and the anisotropy split together. Called unconditionally (NOT HAS_NORMAL-
// gated like spec-AA) so coat roughening also applies to meshes without a normal map.
void GE_ApplyCoatRoughening(inout SurfaceOutput so)
{
    float ccAlpha2 = so.clearCoatRoughness * so.clearCoatRoughness; // coat alpha
    ccAlpha2      *= ccAlpha2;                                       // alpha² (variance space)
    so.roughness   = GE_SpecularAAFloor(so.roughness, so.clearCoat * ccAlpha2);
}
#endif

#endif // GE_SPECULAR_AA_GLSL
