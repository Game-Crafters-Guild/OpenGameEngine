// SDF math and evaluation functions for the UI fragment shader.
// Requires ui_sdf_common.glsl to be included first.

#ifndef SDF_FUNCTIONS_GLSL
#define SDF_FUNCTIONS_GLSL

// --- Shadow quality tuning ---
const bool kEnableShadowTwoSampleQuality = true;
const float kShadowTwoSampleOffsetPx = 0.5;

// --- Signed distance to a rounded rectangle with elliptical corners ---
// Returns positive inside, negative outside, zero on boundary.
// Per-corner semi-axes: radX/radY = (TL, TR, BR, BL), horizontal/vertical.
//
// css-backgrounds-3 §5.1 makes a corner an ELLIPSE, and the distance to an
// ellipse has no closed form. Equal semi-axes therefore keep the exact circular
// expression — a circular corner still evaluates the arithmetic it always did —
// and unequal ones take the gradient-normalised first-order distance, which is
// exact along both axes and collapses algebraically to that same circular
// expression as the axes converge, so the two arms meet continuously.
//
// A corner with either semi-axis at zero is squared off here rather than at the
// callers (§5.1 says a zero radius squares the corner). Enforcing it in one
// place is what lets the elliptical arm below assume both axes are positive.
float roundedRectSDF(vec2 p, vec4 rect, vec4 radX, vec4 radY)
{
    vec2 center = rect.xy + rect.zw * 0.5;
    vec2 halfExt = rect.zw * 0.5;
    vec2 q = abs(p - center) - halfExt;

    bool isLeft = p.x < center.x;
    bool isTop  = p.y < center.y;
    vec2 r = vec2(isLeft ? (isTop ? radX.x : radX.w) : (isTop ? radX.y : radX.z),
                  isLeft ? (isTop ? radY.x : radY.w) : (isTop ? radY.y : radY.z));
    // Per axis now: this evaluates one corner per quadrant, so a semi-axis past
    // the half-box has no arc left to draw. UsedBorderRadius clamps to the same
    // bound on the CPU and the two have to stay in step.
    r = min(r, halfExt);
    if (r.x <= 0.0 || r.y <= 0.0)
        r = vec2(0.0);

    vec2 d = q + r;
    vec2 md = max(d, vec2(0.0));

    // The corner-free core, where the nearest straight edge is the answer and
    // the radius cancels out of the circular form — so both arms share it.
    if (md.x <= 0.0 && md.y <= 0.0)
        return -max(q.x, q.y);

    if (r.x == r.y)
        return -(length(md) - r.x);

    // |md/r| = 1 is the ellipse; dividing that implicit value by its gradient
    // magnitude turns it into a distance. md is non-zero on this path, so k2
    // cannot vanish while both semi-axes are positive.
    float k1 = length(md / r);
    float k2 = length(md / (r * r));
    return -((k1 - 1.0) * k1 / max(k2, 1e-12));
}

// Screen-pixel footprint, in SDF units, of the fragment at local position `p`.
//
// The AA width has to come from the interpolant, not from the distance field.
// `p` varies linearly over the primitive, so its 2x2-quad derivatives are the
// exact screen-to-local scale. The field's own derivatives are not: a
// rounded-rect SDF folds on the rect's centre axes and a border band's field
// ridges along the band centreline, so the quad finite difference of it
// collapses to zero wherever a quad straddles the ridge and stretches by up to
// sqrt(2) across a corner arc. That made the AA width — and with it the
// coverage — depend on where the even-aligned quad grid happened to fall
// relative to the shape, which is why the horizontal and vertical edges of one
// rect resolved to visibly different brightness.
float pixelFootprint(vec2 p)
{
    vec2 dpdx = dFdx(p);
    vec2 dpdy = dFdy(p);
    return max(sqrt(abs(dpdx.x * dpdy.y - dpdx.y * dpdy.x)), 1e-4);
}

// Box-filter coverage of the half-plane at signed distance `sd` (positive
// inside) for a fragment whose pixel footprint spans `w` SDF units.
//
// Linear in sd, which is what makes it exact for a straight edge at every
// sub-pixel phase and makes a band's coverage taken as
// coverage(outer) - coverage(inner) conserve area exactly, down to widths
// below one pixel. A smoothstep ramp conserves neither: it over-counts a
// straight edge by ~16% when the edge lands on a pixel centre and is exact
// when it lands on a pixel boundary, so the same border rendered on two
// differently-phased edges of one rect carries different total ink.
float sdfCoverage(float sd, float w)
{
    return clamp(sd / w + 0.5, 0.0, 1.0);
}

// --- Gaussian shadow (CSS box-shadow) ---

// Abramowitz & Stegun approximation of erf(x), max error ~1.5e-7.
float erfApprox(float x)
{
    float s = sign(x);
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t
                      - 0.284496736) * t + 0.254829592) * t * exp(-a * a);
    return s * y;
}

// Gaussian shadow falloff (sigma = blurRadius / 2).
//
// The pixel term is the fragment's screen footprint expressed in the units sd
// is measured in — the same quantity the coverage box filter uses, so the
// compat arm reads it from the hoisted derivatives rather than re-differencing
// a field whose quad gradient collapses along its own ridges.
float gaussianShadowAlphaIntegrated(float sd, float blurRadius)
{
    float sigma = max(blurRadius * 0.5, 0.001);
#if defined(GE_COMPAT_PROFILE)
    float pixelSigma = max(0.5 * ge_UiDeriv.LocalPerPixel, 0.0);
#else
    float pixelSigma = max(0.5 * fwidth(sd), 0.0);
#endif
    float sigmaEff = sqrt(sigma * sigma + pixelSigma * pixelSigma);
    return 0.5 + 0.5 * erfApprox(sd / (sigmaEff * 1.4142135));
}

// Symmetric taps around center sample for quality.
float evaluateShadowAlpha(float sd, float blurRadius)
{
    float center = gaussianShadowAlphaIntegrated(sd, blurRadius);

    if (!kEnableShadowTwoSampleQuality)
        return center;

#if defined(GE_COMPAT_PROFILE)
    float gradLen = ge_UiDeriv.LocalPerPixel;
#else
    float gradLen = length(vec2(dFdx(sd), dFdy(sd)));
#endif
    if (gradLen <= 1e-5)
        return center;

    float delta = gradLen * kShadowTwoSampleOffsetPx;
    float minusA = gaussianShadowAlphaIntegrated(sd - delta, blurRadius);
    float plusA = gaussianShadowAlphaIntegrated(sd + delta, blurRadius);
    return (center + minusA + plusA) / 3.0;
}

// --- Cubic bezier distance ---
// Evaluate cubic bezier B(t) = (1-t)^3*P0 + 3(1-t)^2*t*P1 + 3(1-t)*t^2*P2 + t^3*P3.
vec2 cubicBezier(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t)
{
    float u = 1.0 - t;
    float u2 = u * u;
    float t2 = t * t;
    return u2 * u * p0 + 3.0 * u2 * t * p1 + 3.0 * u * t2 * p2 + t2 * t * p3;
}

vec2 cubicBezierDeriv(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t)
{
    float u = 1.0 - t;
    return 3.0 * (u * u * (p1 - p0) + 2.0 * u * t * (p2 - p1) + t * t * (p3 - p2));
}

vec2 cubicBezierSecondDeriv(vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t)
{
    return 6.0 * ((1.0 - t) * (p2 - 2.0 * p1 + p0) + t * (p3 - 2.0 * p2 + p1));
}

// Newton-refine a candidate t for closest point on cubic bezier to p.
float newtonRefineBezier(vec2 p, vec2 p0, vec2 p1, vec2 p2, vec2 p3, float t)
{
    for (int i = 0; i < 4; ++i)
    {
        vec2 bp = cubicBezier(p0, p1, p2, p3, t);
        vec2 d1 = cubicBezierDeriv(p0, p1, p2, p3, t);
        vec2 d2 = cubicBezierSecondDeriv(p0, p1, p2, p3, t);
        vec2 diff = bp - p;
        float num = dot(diff, d1);
        float den = dot(d1, d1) + dot(diff, d2);
        if (abs(den) > 1e-6)
            t = clamp(t - num / den, 0.0, 1.0);
    }
    return length(cubicBezier(p0, p1, p2, p3, t) - p);
}

// Distance from point p to a cubic bezier curve.
// Scans 24 uniform samples to detect every local minimum of ||B(t)-p||^2,
// then Newton-refines each one. Also always refines the global-best sample
// as a safety net. Handles S-curves and multi-modal distance functions that
// single-candidate solvers get wrong.
float cubicBezierDist(vec2 p, vec2 p0, vec2 p1, vec2 p2, vec2 p3)
{
    const int kCoarseSamples = 24;

    float best = min(length(p - p0), length(p - p3));

    float globalBestT = 0.0;
    float globalBestD2 = dot(p0 - p, p0 - p);

    float prevPrevD2 = 1e20;
    float prevD2 = globalBestD2;

    for (int i = 1; i <= kCoarseSamples; ++i)
    {
        float t = float(i) / float(kCoarseSamples);
        vec2 bp = cubicBezier(p0, p1, p2, p3, t);
        float d2 = dot(bp - p, bp - p);

        if (d2 < globalBestD2)
        {
            globalBestD2 = d2;
            globalBestT = t;
        }

        if (prevD2 <= prevPrevD2 && prevD2 <= d2)
        {
            float candT = float(i - 1) / float(kCoarseSamples);
            best = min(best, newtonRefineBezier(p, p0, p1, p2, p3, candT));
        }

        prevPrevD2 = prevD2;
        prevD2 = d2;
    }

    // Catch minimum at the tail end of the curve (d2 still decreasing at t=1).
    if (prevD2 <= prevPrevD2)
        best = min(best, newtonRefineBezier(p, p0, p1, p2, p3, 1.0));

    // Always refine the global-best coarse sample as a fallback.
    best = min(best, newtonRefineBezier(p, p0, p1, p2, p3, globalBestT));

    return best;
}

// --- HSV conversion ---
vec3 hsvToRgb(float hueDeg, float sat, float val)
{
    float h = fract(hueDeg / 360.0);
    vec3 k = vec3(1.0, 2.0 / 3.0, 1.0 / 3.0);
    vec3 p = abs(fract(vec3(h) + k) * 6.0 - 3.0);
    return val * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), clamp(sat, 0.0, 1.0));
}

// --- Gradient evaluation ---
vec4 computeFillColor(vec2 p, vec4 rect, uint gradMode,
                      vec4 fillColor, uvec4 packed, uint borderColorPacked)
{
    if (gradMode == GRAD_VERTICAL)
    {
        vec4 c0 = uiPaintColor(packed.z);
        vec4 c1 = uiPaintColor(packed.w);
        float t = (rect.w > 0.0) ? clamp((p.y - rect.y) / rect.w, 0.0, 1.0) : 0.0;
        return mix(c0, c1, t);
    }
    if (gradMode == GRAD_HORIZONTAL)
    {
        vec4 c0 = uiPaintColor(packed.z);
        vec4 c1 = uiPaintColor(packed.w);
        float t = (rect.z > 0.0) ? clamp((p.x - rect.x) / rect.z, 0.0, 1.0) : 0.0;
        return mix(c0, c1, t);
    }
    if (gradMode == GRAD_FOUR_CORNER)
    {
        vec4 cTR = uiPaintColor(packed.z);
        vec4 cBL = uiPaintColor(packed.w);
        vec4 cBR = uiPaintColor(borderColorPacked);
        float u = (rect.z > 0.0) ? clamp((p.x - rect.x) / rect.z, 0.0, 1.0) : 0.0;
        float v = (rect.w > 0.0) ? clamp((p.y - rect.y) / rect.w, 0.0, 1.0) : 0.0;
        vec4 top = mix(fillColor, cTR, u);
        vec4 bot = mix(cBL, cBR, u);
        return mix(top, bot, v);
    }
    if (gradMode == GRAD_POLAR_HSV)
    {
        vec2 center = rect.xy + rect.zw * 0.5;
        float radius = max(min(rect.z, rect.w) * 0.5, 0.0001);
        vec2 delta = p - center;
        float sat = clamp(length(delta) / radius, 0.0, 1.0);
        float hue = degrees(atan(delta.y, delta.x));
        if (hue < 0.0) hue += 360.0;
        vec3 rgb = hsvToRgb(hue, sat, 1.0);
        return uiPaintColor(vec4(rgb, fillColor.a));
    }
    if (gradMode == GRAD_POLAR_HSV_GRADING)
    {
        // Grading trackball (Unity SMH, Unreal grading wheels): a hue ANNULUS with
        // a fully TRANSPARENT hollow center — the inspector panel shows through the
        // interior. Only a thin rim band carries fully-saturated hue; the puck's
        // distance from center is the STRENGTH of the hue push, and the wheel
        // encodes chroma only (brightness lives on the Value slider below). Alpha 0
        // in the interior makes ui_sdf.frag discard those fragments (its fill-alpha
        // path composites this return value, so no extra plumbing is needed).
        vec2 center = rect.xy + rect.zw * 0.5;
        float radius = max(min(rect.z, rect.w) * 0.5, 0.0001);
        vec2 delta = p - center;
        float r = clamp(length(delta) / radius, 0.0, 1.0);
        float hue = degrees(atan(delta.y, delta.x));
        if (hue < 0.0) hue += 360.0;
        // Thin band near the rim (~10% of the radius) with crisp, lightly AA'd
        // edges on both sides (the disc SDF also AAs the outer circle). r reaches
        // 1.0 at the disc edge.
        const float kRingInner = 0.90;  // inner edge of the ring band
        const float kRingOuter = 0.995; // outer edge, just inside the disc rim
        const float kEdgeAA    = 0.018; // smoothstep half-width (~1px on a 120px disc)
        float aInner = smoothstep(kRingInner - kEdgeAA, kRingInner + kEdgeAA, r);
        float aOuterEdge = 1.0 - smoothstep(kRingOuter - kEdgeAA, kRingOuter + kEdgeAA, r);
        float ringAlpha = aInner * aOuterEdge;
        vec3 rgb = hsvToRgb(hue, 1.0, 1.0);
        return uiPaintColor(vec4(rgb, ringAlpha * fillColor.a));
    }
    if (gradMode == GRAD_HUE_VERT)
    {
        float t = (rect.w > 0.0) ? clamp((p.y - rect.y) / rect.w, 0.0, 1.0) : 0.0;
        vec3 rgb = hsvToRgb(t * 360.0, 1.0, 1.0);
        return uiPaintColor(vec4(rgb, fillColor.a));
    }
    return fillColor;
}

// --- Slug text (direct Bezier curve evaluation) ---
#include "slug_functions.glsl"

#endif // SDF_FUNCTIONS_GLSL
