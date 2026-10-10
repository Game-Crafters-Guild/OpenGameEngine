// Compat-profile screen-space rates for the UI SDF fragment shader.
//
// WGSL rejects an implicit derivative — and an implicit-LOD texture sample —
// reached through non-uniform control flow, and every one of ui_sdf.frag's mode
// branches is non-uniform, as is the texture-slot switch inside them. Hoisting
// is sound rather than approximate for the interpolant derivatives: vLocal and
// vUV vary LINEARLY over a primitive, so their screen derivatives are the same
// wherever inside main they are taken. Sampling then uses explicit gradients
// (textureGrad), which keeps mip selection and anisotropy intact — an explicit
// LOD 0 would have thrown both away.
//
// The remaining sites differentiate a computed distance field. For a
// unit-gradient SDF expressed in vLocal units, that screen gradient IS
// LocalPerPixel — which is the substitution the rect and line paths already
// make deliberately, because a field's own quad finite-difference collapses
// along its ridges (see pixelFootprint in sdf_functions.glsl). The compat arms
// therefore do not lose a term; they take the better one.
#ifndef UI_DERIVATIVES_GLSL
#define UI_DERIVATIVES_GLSL

#if defined(GE_COMPAT_PROFILE)

struct UiDerivatives
{
    float LocalPerPixel; // vLocal units spanned by one screen pixel
    vec2  UvDdx;         // dFdx(vUV) — texture gradient / subpixel stripe basis
    vec2  UvDdy;         // dFdy(vUV) — texture gradient
};

// Written once at the top of main, in uniform control flow; read from the mode
// branches and from the leaf helpers they call.
UiDerivatives ge_UiDeriv;

// fwidth(vUV), rebuilt from the hoisted partials — glslang expands fwidth to
// exactly this sum, so the Slug em-space footprint is unchanged.
vec2 ge_UiUvFwidth()
{
    return abs(ge_UiDeriv.UvDdx) + abs(ge_UiDeriv.UvDdy);
}

#endif // GE_COMPAT_PROFILE

#endif // UI_DERIVATIVES_GLSL
