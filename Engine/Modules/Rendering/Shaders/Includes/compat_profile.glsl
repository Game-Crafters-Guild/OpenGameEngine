// Compat-profile (WebGPU/WGSL) sampling and derivative fallbacks.
//
// WGSL requires uniform control flow for every implicit-derivative operation
// (textureSample, dpdx/dpdy) and for comparison sampling; the engine's
// lighting loops branch per light/cascade around exactly those calls. The
// compat profile substitutes explicit-LOD taps (shadow atlases are
// single-mip, so lod 0 is exact) and flat derivatives (quality trade:
// derivative-driven filters lose their slope term; revisit by hoisting
// derivatives to uniform flow if it shows).
#ifndef GE_COMPAT_PROFILE_GLSL
#define GE_COMPAT_PROFILE_GLSL

#if defined(GE_COMPAT_PROFILE)
#extension GL_EXT_texture_shadow_lod : require
#define GE_SHADOW_TAP(s, coords) textureLod(s, coords, 0.0)
#define GE_TAP_LOD0(s, coords) textureLod(s, coords, 0.0)
#define GE_DFDX(v) ((v) * 0.0)
#define GE_DFDY(v) ((v) * 0.0)
// naga drops the array layer count from imageSize; callers supply their bound layer count.
#define GE_ARRAY_IMAGE_SIZE(img, layers) ivec3(imageSize(img).xy, layers)
// WGSL has no isnan; NaN != NaN is the same predicate, but it is a different
// SPIR-V op (OpFUnordNotEqual, not OpIsNan) that a fast-math driver may fold
// away, so only the compat arm takes it.
#define GE_IS_NAN(v) notEqual(v, v)
#else
#define GE_SHADOW_TAP(s, coords) texture(s, coords)
#define GE_TAP_LOD0(s, coords) texture(s, coords)
#define GE_DFDX(v) dFdx(v)
#define GE_DFDY(v) dFdy(v)
#define GE_ARRAY_IMAGE_SIZE(img, layers) imageSize(img)
#define GE_IS_NAN(v) isnan(v)
#endif

#endif // GE_COMPAT_PROFILE_GLSL
