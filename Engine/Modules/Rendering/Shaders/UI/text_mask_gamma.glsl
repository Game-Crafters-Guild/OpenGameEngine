// Colour-keyed text coverage correction, after Skia's SkMaskGamma.
//
// Glyph coverage out of the Slug rasteriser is area-exact (measured: GPU ink vs
// a 16x-supersampled FreeType reference is 1.0056 at 12 px Regular, 0.9925 at
// 14 px Bold). What is not exact is the *blend*. Skia splits the correction for
// that into two independent terms and so does this file.
//
// 1. Blend-space retarget, bidirectional. An attachment interpolates either
//    LINEAR values (a linear intermediate; the terminal encode owns the OETF)
//    or raw sRGB-ENCODED bytes (the browser blend model: source-over on UNORM
//    bytes holding encoded values). The retarget solves, per fragment, for the
//    coverage whose blend in the attachment's ACTUAL space (blendSpaceEncoded)
//    lands where a blend in the TARGET space (blendGamma) would have put it:
//
//      linear  attachment, linear  target  ->  identity (the shipped default)
//      linear  attachment, encoded target  ->  the "gamma-space blending" look
//                                              out of a linear compositor
//      encoded attachment, linear  target  ->  Skia's own direction: the
//                                              pre-distortion that makes an
//                                              encoded source-over land
//                                              area-exact
//      encoded attachment, encoded target  ->  identity, by the same maths
//
//    Skia's LUT is the third case built with SK_GAMMA_SRGB — what Chrome ships
//    on Windows and macOS: physically area-exact ink out of an encoded blit.
//    The retarget is never a thinning exponent, and a faithful port must not
//    smuggle one in; blendGamma names the TARGET space only (0 = linear,
//    1.0 = display-encoded), while the actual space is a fact of the
//    attachment, resolved by UIManager::ResolveTextCoverageConstants.
//
// 2. Contrast boost, keyed on the paint colour. Skia, verbatim in shape:
//        adjustedContrast = contrast * linDst;
//        srca += (1 - srca) * adjustedContrast * srca;
//    with the destination *guessed* as the paint's perceptual inverse
//    (dst = 1 - src). That guess is what lets the correction run without a
//    framebuffer read, and it is the approximation this file inherits — both
//    terms share it. The boost tapers to nothing as text approaches white and
//    reaches full strength for near-black text — the polarity asymmetry that
//    the size-keyed exponent this replaced could not express, because a power
//    curve thins BOTH polarities.
//
// Both terms are keyed on the glyph's luminance and never on ppem: identical
// glyph shapes must not gain or lose ink for having been asked for at a
// different size.

#ifndef UI_TEXT_MASK_GAMMA_GLSL
#define UI_TEXT_MASK_GAMMA_GLSL

const vec3 kRec709Luma = vec3(0.2126, 0.7152, 0.0722);

// blendGamma sentinel: the target blend space is sRGB-linear. For a linear
// attachment the retarget is then identity and the contrast boost is the whole
// correction; for an encoded attachment it selects the FULL Skia-direction
// correction (area-exact ink out of an encoded blend).
const float kTextBlendGammaLinearTarget = 0.0;

// Below this |srcLin - dstLin| the linear-arm inversion is 0/0. Skia contains
// the same instability with a 1/256 guard on |src - dst|; this is that guard
// carried into linear units, where the encoded midpoint's slope is ~0.64.
const float kTextSpanFadeEnd = 1.0 / 128.0;

// The encoded-arm twin of kTextSpanFadeEnd: same 2x-Skia-guard fade width, in
// the units this arm divides in — encoded — where the 1/256 guard applies
// natively, no slope carry. Both arms are unstable at the same glyph colour
// (mid-grey against its own inverse, src = 0.5 encoded).
const float kTextSpanFadeEndEncoded = 1.0 / 128.0;

float srgbEncodeScalar(float linear)
{
    float c = clamp(linear, 0.0, 1.0);
    return (c <= 0.0031308) ? (c * 12.92) : (1.055 * pow(c, 1.0 / 2.4) - 0.055);
}

float srgbDecodeScalar(float encoded)
{
    float c = clamp(encoded, 0.0, 1.0);
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

// Skia's apply_contrast(). Fixes both endpoints and bulges the middle, so it
// adds weight to partially covered pixels without leaking outside the glyph.
float applyTextContrast(float srca, float adjustedContrast)
{
    return srca + (1.0 - srca) * adjustedContrast * srca;
}

// The correction is keyed on the glyph paint's LINEAR luminance in every
// pipeline variant; the paint arrives in the variant's BLEND space (linear by
// default, raw sRGB bytes under UI_BLEND_SPACE_ENCODED), so the encoded
// variant decodes here — for the KEY only. Decoding per channel before the
// luma dot keeps the key bit-comparable across variants: the same glyph
// colour selects the same correction whichever space it blends in.
float textGlyphLinearLuma(vec3 glyphPaint)
{
#ifdef UI_BLEND_SPACE_ENCODED
    return dot(vec3(srgbDecodeScalar(glyphPaint.r), srgbDecodeScalar(glyphPaint.g),
                    srgbDecodeScalar(glyphPaint.b)),
               kRec709Luma);
#else
    return dot(clamp(glyphPaint, vec3(0.0), vec3(1.0)), kRec709Luma);
#endif
}

// Where the TARGET-space blend of boosted coverage `a` lands, in linear light.
// src/dst are the guessed encoded endpoints, srcLin/dstLin their linear twins.
float textTargetBlendLinear(float a, float src, float dst, float srcLin, float dstLin,
                            float blendGamma)
{
    if (blendGamma <= kTextBlendGammaLinearTarget)
        return mix(dstLin, srcLin, a);
    float srcG = pow(src, blendGamma);
    float dstG = pow(dst, blendGamma);
    return srgbDecodeScalar(pow(mix(dstG, srcG, a), 1.0 / blendGamma));
}

// coverage           area-exact glyph coverage.
// glyphPaint         the glyph's rgb in the variant's BLEND space (see
//                    textGlyphLinearLuma).
// contrast           Skia's SK_GAMMA_CONTRAST. Chrome ships 1.0 on Windows;
//                    0 disables.
// blendGamma         kTextBlendGammaLinearTarget, or the exponent of the
//                    TARGET space the coverage should blend linearly in
//                    (1.0 = display-encoded).
// blendSpaceEncoded  0: the attachment interpolates LINEAR values.
//                    1: the attachment interpolates raw sRGB-ENCODED bytes.
float correctTextCoverage(float coverage, vec3 glyphPaint, float contrast, float blendGamma,
                          int blendSpaceEncoded)
{
    // srcLin is the linear level the correction models; src is its
    // display-encoded level, which is what Skia keys on because that is where
    // the guess below lives.
    float srcLin = textGlyphLinearLuma(glyphPaint);
    float src = srgbEncodeScalar(srcLin);
    float dst = 1.0 - src;
    float dstLin = srgbDecodeScalar(dst);

    float a = applyTextContrast(coverage, contrast * dstLin);

    if (blendSpaceEncoded == 0)
    {
        if (blendGamma <= kTextBlendGammaLinearTarget)
            return a;

        // Retarget: solve for the coverage whose LINEAR blend lands where a
        // blend in the blendGamma space would have put it.
        float wantedLin = textTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
        float span = srcLin - dstLin;
        // span -> 0 is mid-grey text against its own inverse; the inversion's
        // limit there is `a`, so fade to that rather than dividing by nothing.
        // The guard must sit on the DENOMINATOR: at span == 0 the quotient is
        // 0/0, and NaN survives a zero mix weight. Any nonzero span clamps
        // finite, so exact zero is the one case to reroute.
        float safeSpan = (span == 0.0) ? 1.0 : span;
        return mix(a, clamp((wantedLin - dstLin) / safeSpan, 0.0, 1.0),
                   smoothstep(0.0, kTextSpanFadeEnd, abs(span)));
    }

    // Skia-direction arm: solve for the coverage whose ENCODED source-over
    // lands where a blend in the blendGamma space would have put it. At the
    // linear-target sentinel this is the full area-exact correction; at
    // blendGamma 1.0 (the encoded space itself) it is the identity — each
    // arm's identity sits at its own blend space, which is what makes the
    // retarget bidirectional rather than a pair of special cases.
    float wantedLin = textTargetBlendLinear(a, src, dst, srcLin, dstLin, blendGamma);
    float spanEnc = src - dst;
    // Same denominator guard as the linear arm: 0/0 at the exact inverse
    // poisons the mix even at weight zero.
    float safeSpanEnc = (spanEnc == 0.0) ? 1.0 : spanEnc;
    return mix(a, clamp((srgbEncodeScalar(wantedLin) - dst) / safeSpanEnc, 0.0, 1.0),
               smoothstep(0.0, kTextSpanFadeEndEncoded, abs(spanEnc)));
}

#ifdef UI_SUBPIXEL_DUAL_SRC
// Per-channel form for subpixel RGB coverage: one correction per channel, all
// keyed on the SAME paint luminance — subpixel AA moves where coverage is
// sampled, not what colour the correction believes it is drawing. A per-channel
// key would re-weight the R/G/B fringes against each other and tint the glyph.
vec3 correctTextCoverageRGB(vec3 coverage, vec3 glyphPaint, float contrast, float blendGamma,
                            int blendSpaceEncoded)
{
    return vec3(correctTextCoverage(coverage.r, glyphPaint, contrast, blendGamma, blendSpaceEncoded),
                correctTextCoverage(coverage.g, glyphPaint, contrast, blendGamma, blendSpaceEncoded),
                correctTextCoverage(coverage.b, glyphPaint, contrast, blendGamma, blendSpaceEncoded));
}
#endif // UI_SUBPIXEL_DUAL_SRC

#endif // UI_TEXT_MASK_GAMMA_GLSL
