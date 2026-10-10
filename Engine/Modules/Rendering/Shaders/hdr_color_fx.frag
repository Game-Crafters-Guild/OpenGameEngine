#version 450

// Combined HDR color effects before tonemap — the grade block. Composition
// order follows URP's LutBuilderHdr chain (the industry reference the authoring
// model mirrors):
//   1. white balance   — von Kries gains in URP's LMS space, applied in LINEAR.
//                        (A log-space formulation would be identical in the log
//                        body — log2(k*x) = log2 k + log2 x — but would tint
//                        scene blacks through the encode toe; URP is linear.)
//   2. color filter    — linear blend against the tint (URP's filter is an
//                        unclipped multiply applied before its SMH trackballs).
//   3. three-way corrector in grade-log space (SMH offsets -> contrast ->
//                        saturation; shipped slice-1/2 structure, unchanged).
//   4. hue shift       — HSV rotation on the graded linear color (URP's HSV ops
//                        close its chain too).
// Documented deviations from URP, both inherited from the unified corrector:
//   - URP applies log contrast BEFORE its color filter; our contrast lives
//     inside the corrector, so with contrast != 1 the filter push is
//     contrast-scaled here.
//   - URP rotates hue BEFORE global saturation; ours saturates inside the
//     corrector, so hue rotates the already-saturated color. Hue rotation
//     ~commutes with luma-lerp saturation; the residual is second-order.
//
// Working space: the render's Rec.709-LINEAR RGB (the same primaries the scene
// is lit and tonemapped in). The three-way grade runs in a log-encoded copy of
// that space (ACEScct-shaped); it is NOT a wide-gamut (AP1/ACEScg) grade. A
// future advanced option would gamut-map Rec.709 -> AP1 around the log block
// and back; hook left below (GRADE_WORKING_SPACE) — not implemented.
//
// The whole block is anchored POST-EXPOSURE, like URP's LUT input (URP bakes
// its grade on post-exposure scene color): exposure is multiplied in at the
// top so the log pivot sits at exposed mid-grey (0.18) and the filter/WB see
// display-stable magnitudes, then exposure is divided back out so
// tonemap.frag's single *exposure still lands exactly once. The filter's
// non-multiply blend modes (Add/Screen/SoftLight) therefore compose against
// EXPOSED values — under auto-exposure that is the point (a stable look for a
// stable displayed image); Multiply is exposure-invariant either way.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;

// Metered auto-exposure history (AutoExposureNode). Read the same way tonemap.frag
// reads it so the grade anchors at the exact scalar the tonemap will apply. Bound by
// the rendergraph (ExposureHistory); a zero placeholder falls back to pc.exposure.
layout(set = 0, binding = 1, std430) readonly buffer ExposureHistory {
    float exposureScale;
    uint  valid;
} uExposure;

// std140 grade block (ColorGradeParamsUBO). Zero-filled == identity grade. No
// instance name so the reflected binding is the block name "ColorGradeParams"
// (matches the rendergraph buffer key, per the ViewParams convention).
layout(set = 0, binding = 2, std140) uniform ColorGradeParams {
    vec4 uShadows;    // rgb chroma offset, master lightness in .w
    vec4 uMidtones;
    vec4 uHighlights;
    vec4 uGlobals;    // contrast-1, saturation-1, gradeInLinear, pad
    // Band limits as deltas from the kDefault* constants below (zero == default
    // partition): shadowsStart, shadowsEnd, highlightsStart, highlightsEnd.
    // Sanitized CPU-side (FillColorGradeParamsUBO): start in [0,1], end > start.
    vec4 uBandLimits;
    // White balance LMS gains minus 1 (CPU: URP ColorBalanceToLMSCoeffs from
    // temperature/tint; exactly zero at neutral), hue rotation in turns in .w.
    vec4 uWhiteBalance;
};

layout(push_constant) uniform ColorFxPC
{
    float colorFilterR;
    float colorFilterG;
    float colorFilterB;
    float colorFilterIntensity;
    int colorFilterBlendMode;
    float exposure;       // static/manual/physical scalar (matches tonemap.frag)
    int useAutoExposure;  // 1 = read metered exposure from uExposure
} pc;

const vec3 kLumaWeights = vec3(0.2126, 0.7152, 0.0722);
// Scales the stored band offsets to a calm push (stored offsets are wheel-space,
// up to ~1.0 at full saturation; a direct add would over-drive the log axis).
const float kGradeOffsetScale = 0.5;

// Default band partition on the encoded-log axis. Mirror of kColorGrade*Default
// in ColorGradeParamsUBO.h — keep in lockstep (a zero-filled uBandLimits decodes
// to exactly these). Exposed mid-grey encodes to ~0.414; encoded 0.95 is exposed
// linear ~120, so full highlight weight is still reserved for near-clipping
// pixels while the ramp starts just above mid-grey.
const float kDefaultShadowsStart = 0.0;
const float kDefaultShadowsEnd = 0.45;
const float kDefaultHighlightsStart = 0.45;
const float kDefaultHighlightsEnd = 0.95;

// URP's "sharpened" linear<->LMS pair (Color.hlsl LIN_2_LMS_MAT / LMS_2_LIN_MAT).
// The HLSL sources are row-major; GLSL mat3 constructors fill COLUMNS, so these
// listings are the transposed rows.
const mat3 kLinToLms = mat3(
    3.90405e-1, 7.08416e-2, 2.31082e-2,
    5.49941e-1, 9.63172e-1, 1.28021e-1,
    8.92632e-3, 1.35775e-3, 9.36245e-1);
const mat3 kLmsToLin = mat3(
     2.85847e+0, -2.10182e-1, -4.18120e-2,
    -1.62879e+0,  1.15820e+0, -1.18169e-1,
    -2.48910e-2,  3.24281e-4,  1.06867e+0);

// Von Kries adaptation with the CPU-resolved gains (URP WhiteBalance parity).
vec3 ApplyWhiteBalance(vec3 c)
{
    vec3 lms = kLinToLms * c;
    lms *= uWhiteBalance.xyz + 1.0;
    return kLmsToLin * lms;
}

// Hocevar branchless RGB<->HSV — the same construction URP's Color.hlsl uses.
// Hue is in turns [0,1); both are scale-invariant, so HDR magnitudes pass
// through V untouched.
vec3 RgbToHsv(vec3 c)
{
    vec4 K = vec4(0.0, -1.0 / 3.0, 2.0 / 3.0, -1.0);
    vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
    vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
    float d = q.x - min(q.w, q.y);
    float e = 1.0e-10;
    return vec3(abs(q.z + (q.w - q.y) / (6.0 * d + e)), d / (q.x + e), q.x);
}

vec3 HsvToRgb(vec3 c)
{
    vec4 K = vec4(1.0, 2.0 / 3.0, 1.0 / 3.0, 3.0);
    vec3 p = abs(fract(c.xxx + K.xyz) * 6.0 - K.www);
    return c.z * mix(K.xxx, clamp(p - K.xxx, 0.0, 1.0), c.y);
}

// GLSL fract() maps negatives into [0,1), so a negative shift wraps correctly.
vec3 ApplyHueShift(vec3 c)
{
    vec3 hsv = RgbToHsv(c);
    hsv.x = fract(hsv.x + uWhiteBalance.w);
    return HsvToRgb(hsv);
}

vec3 ApplyColorFilterBlend(vec3 src, vec3 tint, int mode)
{
    if (mode == 1)
        return src + tint;
    if (mode == 2)
        return 1.0 - (1.0 - src) * (1.0 - tint);
    if (mode == 3)
    {
        vec3 low  = src - (1.0 - 2.0 * tint) * src * (1.0 - src);
        vec3 g    = mix(((16.0 * src - 12.0) * src + 4.0) * src, sqrt(max(src, vec3(0.0))), step(vec3(0.25), src));
        vec3 high = src + (2.0 * tint - 1.0) * (g - src);
        bvec3 useHigh = greaterThan(tint, vec3(0.5));
        return mix(low, high, useHigh);
    }
    return src * tint;
}

// GradeLog: ACEScct-shaped (linear toe + log2 body). Mirror of EncodeGradeLog /
// DecodeGradeLog in Engine/Include/Engine/Rendering/ColorGradeParamsUBO.h — keep
// the constants in lockstep. Never bare log2: log2(0) = -inf -> NaN on scene blacks.
vec3 EncodeGradeLog(vec3 x)
{
    vec3 toe = x * 10.5402 + 0.0729;
    vec3 body = (log2(max(x, vec3(1e-10))) + 9.72) / 17.52;
    return mix(body, toe, lessThanEqual(x, vec3(0.0078125)));
}

vec3 DecodeGradeLog(vec3 y)
{
    vec3 toe = (y - 0.0729) / 10.5402;
    vec3 body = exp2(y * 17.52 - 9.72);
    return mix(body, toe, lessThanEqual(y, vec3(0.1554)));
}

// Unified three-way corrector in log space: smooth luminance-weighted (SMH) band
// masks -> per-band offset (chroma + master) -> global contrast -> global saturation.
// At default (contrast 1 / sat 1 / zero offsets) this is an exact pass-through.
vec3 GradeLogSpace(vec3 logColor)
{
    float logLum = dot(logColor, kLumaWeights);
    float wShadow    = 1.0 - smoothstep(kDefaultShadowsStart + uBandLimits.x,
                                        kDefaultShadowsEnd + uBandLimits.y, logLum);
    float wHighlight = smoothstep(kDefaultHighlightsStart + uBandLimits.z,
                                  kDefaultHighlightsEnd + uBandLimits.w, logLum);
    // Partition of unity for ANY authored limits: when the shadow and highlight
    // ramps overlap their sum can exceed 1 — renormalize so wS + wH <= 1 and the
    // midtone remainder can never go negative (the max() is float-noise armor).
    float wNorm = max(wShadow + wHighlight, 1.0);
    wShadow /= wNorm;
    wHighlight /= wNorm;
    float wMid = max(1.0 - wShadow - wHighlight, 0.0);

    vec3 bandSum = wShadow    * (uShadows.rgb    + uShadows.w)
                 + wMid       * (uMidtones.rgb   + uMidtones.w)
                 + wHighlight * (uHighlights.rgb + uHighlights.w);
    logColor += bandSum * kGradeOffsetScale;

    // Contrast about encoded mid-grey (EncodeGradeLog(0.18) ~= 0.4136).
    const float kPivot = 0.4136;
    float contrast = 1.0 + uGlobals.x;
    logColor = (logColor - kPivot) * contrast + kPivot;

    // Saturation: luma-weighted lerp in the log domain.
    float saturation = max(1.0 + uGlobals.y, 0.0);
    float lum = dot(logColor, kLumaWeights);
    logColor = mix(vec3(lum), logColor, saturation);
    return logColor;
}

void main()
{
    vec4 src = texture(uSceneColor, vUV);
    vec3 rgb = src.rgb;

    // Per-lane gates: each stage skips at exact identity, so a zero-filled
    // placeholder UBO (or a filter-only frame) never pays a stage it doesn't
    // use — and the corrector's encode/decode round-trip stays gated exactly
    // as slice 1 shipped it.
    bool wbActive = any(notEqual(uWhiteBalance.xyz, vec3(0.0)));
    bool hueActive = uWhiteBalance.w != 0.0;
    bool correctorActive = any(notEqual(uShadows, vec4(0.0)))
                        || any(notEqual(uMidtones, vec4(0.0)))
                        || any(notEqual(uHighlights, vec4(0.0)))
                        || uGlobals.x != 0.0
                        || uGlobals.y != 0.0;

    // One anchor scalar for the whole block so multiply/divide cancel exactly.
    float e = (pc.useAutoExposure != 0 && uExposure.exposureScale > 0.0 &&
               uExposure.exposureScale < 1e9)
                  ? uExposure.exposureScale
                  : pc.exposure;
    float eDiv = max(e, 1e-4);
    vec3 exposed = max(rgb, vec3(0.0)) * eDiv;

    if (wbActive)
        exposed = ApplyWhiteBalance(exposed);

    // Color filter (post-WB, pre-corrector — URP's filter slot).
    vec3 tint = vec3(pc.colorFilterR, pc.colorFilterG, pc.colorFilterB);
    vec3 filtered = ApplyColorFilterBlend(exposed, tint, pc.colorFilterBlendMode);
    exposed = mix(exposed, filtered, clamp(pc.colorFilterIntensity, 0.0, 1.0));

    // WB out-of-gamut and Add/SoftLight underflow both feed the corrector's log
    // encode; keep it non-negative like URP's max() before its color ops.
    exposed = max(exposed, vec3(0.0));

    if (correctorActive)
    {
        // Linear parity path (uGlobals.z != 0): grade directly on exposed linear.
        vec3 logColor = (uGlobals.z != 0.0) ? exposed : EncodeGradeLog(exposed);
        logColor = GradeLogSpace(logColor);
        exposed = (uGlobals.z != 0.0) ? logColor : DecodeGradeLog(logColor);
    }

    if (hueActive)
        exposed = ApplyHueShift(exposed);

    rgb = exposed / eDiv;  // un-anchor: tonemap re-applies *exposure once

    oColor = vec4(max(rgb, vec3(0.0)), src.a);
}
