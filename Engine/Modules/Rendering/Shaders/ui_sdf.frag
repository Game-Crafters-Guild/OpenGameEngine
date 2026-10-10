#version 450
#extension GL_EXT_nonuniform_qualifier : enable

// Unified UI SDF instanced fragment shader.
// Handles Rect, Slug text, Textured, and Line modes in a single shader.
// Reads UIPrimitive + UIClipRect from SSBOs. Supports bindless textures.

#include "UI/ui_sdf_common.glsl"
#include "UI/ui_derivatives.glsl"
#include "UI/sdf_functions.glsl"
#include "UI/text_mask_gamma.glsl"

layout(location = 0) flat in uint vInstance;
layout(location = 1) in vec2 vLocal;
layout(location = 2) in vec2 vUV;
// Stage 2 part 3: persistent primitive slot resolved by the vertex shader's
// DrawOrder indirection. Must use this to fetch the primitive — reading via
// vInstance (the dense draw index) bypasses the indirection and lands on
// waste / wrong slots in the persistent buffer.
layout(location = 3) flat in uint vSlot;
#if defined(GE_COMPAT_PROFILE)
// Run-local texture slot ordinal, unpacked by the vertex stage from the high
// bits of the draw-order word (UI/ui_sdf_textures.glsl).
layout(location = 4) flat in uint vTexSlot;
#endif
#if defined(UI_SUBPIXEL_DUAL_SRC) && defined(GE_COMPAT_PROFILE)
// WGSL's @blend_src must appear on BOTH members of a dual-source pair, and
// naga only emits it where GLSL spelled an index. Index 0 is the GLSL default,
// so naming it changes nothing about the shader — it is scoped to the profile
// that needs it so the Vulkan path keeps its bytes.
layout(location = 0, index = 0) out vec4 outColor;
#else
layout(location = 0) out vec4 outColor;
#endif
#ifdef UI_SUBPIXEL_DUAL_SRC
// Dual-source blending: the second fragment output carries per-channel alpha
// for ONE / ONE_MINUS_SRC1_COLOR blending. Non-text primitives write
// vec4(alpha) here, which makes their blend arithmetic identical to the
// grayscale pipeline's ONE / ONE_MINUS_SRC_ALPHA.
layout(location = 0, index = 1) out vec4 outCoverage;
#endif

layout(std430, set = 0, binding = 0) readonly buffer PrimBuf {
    UIPrimitive prims[];
} sb;

layout(std430, set = 0, binding = 1) readonly buffer ClipBuf {
    UIClipRect clips[];
} cb;

#include "UI/ui_sdf_textures.glsl"
#include "UI/text_effects.glsl"

// Clip evaluation: walk the parent chain so ancestor rounded corners
// still clip descendants. Typical depth is 1-3; cap at 4 iterations.
const int kMaxClipChainDepth = 4;

// Every primitive exit funnels through here so the dual-source variant can
// never leave its second output undefined (undefined src1 = undefined blend).
void emitPremultiplied(vec4 pm)
{
    outColor = pm;
#ifdef UI_SUBPIXEL_DUAL_SRC
    outCoverage = vec4(pm.a);
#endif
}

// Brightness-neutral edge softness. Symmetric power remap around 0.5 —
// endpoints (0, 1) and midpoint (0.5) are preserved, so total coverage area
// is conserved (no perceived weight change).
//   softness = 1.0 → identity
//   softness > 1.0 → softer (edge values pulled toward 0.5)
//   softness < 1.0 → sharper (edge values pushed toward 0/1)
float applyEdgeSoftness(float coverage)
{
    if (pc.edgeSoftness != 1.0 && pc.edgeSoftness > 0.0)
    {
        float dist = coverage - 0.5;
        coverage = sign(dist) * pow(abs(dist) * 2.0, pc.edgeSoftness) * 0.5 + 0.5;
    }
    return coverage;
}

float computeClipAlpha(vec2 p, uint flags, float px)
{
    uint idx = (flags >> CLIP_INDEX_SHIFT) & 0xFFFFu;
    if (idx == NO_CLIP)
        return 1.0;

    float alpha = 1.0;
#if defined(GE_COMPAT_PROFILE)
    // Keep oversized viewport derivatives from widening browser clip corners.
    px = min(px, 2.0);
#endif
    for (int i = 0; i < kMaxClipChainDepth; ++i)
    {
        UIClipRect clip = cb.clips[idx];
        alpha = min(alpha, sdfCoverage(roundedRectSDF(p, clip.rect, clip.radii, clip.radiiY), px));
        idx = clip.params.x;
        if (idx == NO_CLIP)
            break;
    }
    return alpha;
}

void main()
{
    UIPrimitive prim = sb.prims[vSlot];

    uint flags = prim.packed.x;
    uint mode = flags & MODE_MASK;
    uint gradMode = (flags & GRADIENT_MASK) >> GRADIENT_SHIFT;

    vec4 rect = prim.rectXYWH;
    vec2 p = vLocal;
    float px = pixelFootprint(p);

#if defined(GE_COMPAT_PROFILE)
    // Uniform control flow ends at the first mode branch below, so every
    // implicit derivative the shader needs is taken here (UI/ui_derivatives.glsl).
    ge_UiDeriv.LocalPerPixel = px;
    ge_UiDeriv.UvDdx = dFdx(vUV);
    ge_UiDeriv.UvDdy = dFdy(vUV);
#endif

    float clipA = computeClipAlpha(p, flags, px);
    if (clipA <= 0.0)
        discard;

    float opacity = prim.modeParams.x;

    // Caret blink. The rate comes from the OS caret setting via the push
    // constant; a rate of 0 ("never blink") holds the phase at 0 and so keeps
    // the caret solid without needing a branch of its own.
    float caretAlpha = 1.0;
    if ((flags & CARET_BIT) != 0u)
    {
        float forceUntil = prim.modeParams.z;
        if (pc.timeSeconds >= forceUntil)
        {
            float phase = mod(floor(pc.timeSeconds * pc.caretPhaseTogglesPerSecond), 2.0);
            caretAlpha = (phase < 1.0) ? 1.0 : 0.0;
        }
    }

    // ---- MODE: Rect (with shadow/glow support) ----
    if (mode == MODE_RECT)
    {
        vec4 effects = prim.effects;
        bool insetShadow = (flags & COLOR_GLYPH_BIT) != 0u;
        bool hasEffects = (effects.z > 0.0 || effects.w > 0.0 ||
                           effects.x != 0.0 || effects.y != 0.0 || insetShadow);
        vec4 pad = prim.uvRect;
        vec4 elemRect = hasEffects
            ? vec4(rect.x + pad.x, rect.y + pad.y,
                   rect.z - pad.x - pad.z, rect.w - pad.y - pad.w)
            : rect;

        // Each semi-axis clamps to its own half-extent — a corner is an ellipse,
        // so the horizontal one answers to the width and the vertical to the
        // height (css-backgrounds-3 §5.1).
        float halfW = max(0.0, elemRect.z) * 0.5;
        float halfH = max(0.0, elemRect.w) * 0.5;
        vec4 radX = clamp(prim.radii, vec4(0.0), vec4(halfW));
        vec4 radY = clamp(prim.radiiY, vec4(0.0), vec4(halfH));

        // Shadow. Outer drop-shadows composite behind the fill; inset shadows
        // wait until after fill/border so they sit on the background and under
        // the border, matching CSS box-shadow: inset.
        vec4 shadowCol = uiPaintColor(prim.colors.z);
        float shadowA = 0.0;
        if (shadowCol.a > 0.0 && (effects.z > 0.0 || effects.x != 0.0 || effects.y != 0.0 || insetShadow))
        {
            vec2 shadowP = p - effects.xy;
            float sdShadow = roundedRectSDF(shadowP, elemRect, radX, radY);
            float blur = max(effects.z, 0.5);
            shadowA = evaluateShadowAlpha(sdShadow, blur);
        }

        // Glow (around)
        vec4 glowCol = uiPaintColor(prim.colors.w);
        float glowA = 0.0;
        if (glowCol.a > 0.0 && effects.w > 0.0)
        {
            float sdGlow = roundedRectSDF(p, elemRect, radX, radY);
            float glowR = effects.w;
            float outside = max(-sdGlow, 0.0);
            glowA = exp(-(outside * outside) / (glowR * glowR * 0.5));
            glowA *= (1.0 - sdfCoverage(sdGlow, px));
        }

        // Element fill + border
        float sdOuter = roundedRectSDF(p, elemRect, radX, radY);
        float aOuter = sdfCoverage(sdOuter, px);

        if (!insetShadow)
            shadowA *= (1.0 - aOuter);

        vec4 fillColor = uiPaintColor(prim.colors.x);
        vec4 colFill = computeFillColor(p, elemRect, gradMode, fillColor, prim.packed, prim.colors.y);

        vec4 bw = max(prim.borderWidths, vec4(0.0));
        bool hasBorder = (bw.x + bw.y + bw.z + bw.w) > 0.0;
        vec4 colBorder = (gradMode == GRAD_FOUR_CORNER) ? vec4(0.0) : uiPaintColor(prim.colors.y);

        vec4 result = vec4(0.0);

        if (shadowA > 0.0 && !insetShadow)
            result = vec4(shadowCol.rgb, shadowCol.a * shadowA);

        if (glowA > 0.0)
            result = mix(result, vec4(glowCol.rgb, glowCol.a), glowA);

        if (aOuter > 0.0)
        {
            // Straight-alpha colour + coverage for this element's own paint.
            vec4 elemColor;
            if (!hasBorder || colBorder.a <= 0.0)
            {
                elemColor = vec4(colFill.rgb, colFill.a * aOuter);
            }
            else
            {
                vec4 inner = vec4(elemRect.x + bw.x, elemRect.y + bw.y,
                                  max(0.0, elemRect.z - (bw.x + bw.z)),
                                  max(0.0, elemRect.w - (bw.y + bw.w)));
                // The inner border corner, per axis: the horizontal semi-axis
                // loses the left/right border, the vertical one the top/bottom
                // (bw = L,T,R,B). InnerClipRadii on the CPU derives the overflow
                // clip contour with this exact rule, and the two MUST agree —
                // a clip cutting content along a different arc than the ring it
                // meets leaves a sliver of background at every rounded corner.
                vec4 radInnerX = max(vec4(0.0), radX - vec4(bw.x, bw.z, bw.z, bw.x));
                vec4 radInnerY = max(vec4(0.0), radY - vec4(bw.y, bw.y, bw.w, bw.w));
                float sdInner = roundedRectSDF(p, inner, radInnerX, radInnerY);
                float aInner = sdfCoverage(sdInner, px);
                // Ring coverage as the difference of the two box-filter
                // coverages. Both are linear in their distance field, so the
                // difference is the exact covered area of the band at every
                // sub-pixel phase and for widths under one pixel — and
                // aInner + aBorder is identically aOuter. (Taking the band's
                // own SDF, min(sdOuter, -sdInner), instead re-introduces a
                // ridge along the band centreline, which is precisely the
                // field feature a screen-space derivative cannot resolve.)
                float aBorder = max(aOuter - aInner, 0.0);
                // CSS paints the background to the border-box edge
                // (background-clip: border-box is the default), so inside the
                // ring the border composites source-over ON TOP of the
                // element's own fill. Area-weighting that ring paint over the
                // aInner/aBorder partition keeps aElem == aOuter for opaque
                // paints at the outer AA edge — compositing fill(aOuter)
                // under border(aBorder) as independent layers would
                // double-count the shared outer contour and tint
                // opaque-border fringes with the fill colour. At
                // colBorder.a == 1 the ring terms reduce exactly to the
                // border colour, so opaque borders are bit-identical to a
                // plain fill/border partition.
                float ringA = colBorder.a + colFill.a * (1.0 - colBorder.a);
                vec3 ringPremul = colBorder.rgb * colBorder.a +
                                  colFill.rgb * (colFill.a * (1.0 - colBorder.a));
                float wFill = colFill.a * aInner;
                float aElem = wFill + ringA * aBorder;
                elemColor = vec4(aElem > 0.0
                                     ? (colFill.rgb * wFill + ringPremul * aBorder) / aElem
                                     : vec3(0.0),
                                 aElem);
            }
            if (insetShadow && shadowCol.a > 0.0)
            {
                // Inside the padding box (under the border). Offset-shape
                // coverage is the un-shadowed interior; the rest of the fill
                // receives the inset.
                float inside = aOuter;
                if (hasBorder && colBorder.a > 0.0)
                {
                    vec4 inner = vec4(elemRect.x + bw.x, elemRect.y + bw.y,
                                      max(0.0, elemRect.z - (bw.x + bw.z)),
                                      max(0.0, elemRect.w - (bw.y + bw.w)));
                    vec4 radInnerX = max(vec4(0.0), radX - vec4(bw.x, bw.z, bw.z, bw.x));
                    vec4 radInnerY = max(vec4(0.0), radY - vec4(bw.y, bw.y, bw.w, bw.w));
                    inside = sdfCoverage(roundedRectSDF(p, inner, radInnerX, radInnerY), px);
                }
                float sA = shadowCol.a * (1.0 - shadowA) * inside;
                float outFillA = sA + elemColor.a * (1.0 - sA);
                elemColor.rgb = (outFillA > 0.0)
                    ? (shadowCol.rgb * sA + elemColor.rgb * elemColor.a * (1.0 - sA)) / outFillA
                    : vec3(0.0);
                elemColor.a = outFillA;
            }
            // Source-over in straight-alpha form, matching what `result`
            // holds. Compositing it as `mix(result.rgb, elemColor.rgb,
            // elemColor.a)` left result.rgb already scaled by coverage, and
            // toPremultipliedSdrUi then scaled it by that coverage a second
            // time: partially covered pixels emitted colour * coverage^2 at
            // alpha coverage, i.e. far too dark. That is what notched the
            // rounded corners (arc pixels landing darker than the surface
            // behind them) and what turned a coverage difference between two
            // edges into a much larger brightness difference.
            float outA = elemColor.a + result.a * (1.0 - elemColor.a);
            result.rgb = (outA > 0.0)
                ? (elemColor.rgb * elemColor.a +
                   result.rgb * result.a * (1.0 - elemColor.a)) / outA
                : vec3(0.0);
            result.a = outA;
        }

        result.a *= clipA * caretAlpha * opacity;

        if (result.a <= 0.0)
            discard;

        emitPremultiplied(toPremultipliedSdrUi(result));
        return;
    }

    // ---- MODE: Slug text (direct Bezier curve evaluation) ----
    if (mode == MODE_SLUG)
    {
        bool isColorGlyph = (flags & COLOR_GLYPH_BIT) != 0u;
        vec4 glyphColor = uiPaintColor(prim.colors.x);

        // A glyph with effect fields is the run's effect instance: it draws
        // its shadow, glow and outline only, and the run's plain fill
        // instances draw over it (UI/text_effects.glsl). Plain text never
        // enters the effect path.
        bool hasTextEffects = uiTextEffectReachPx(prim) > 0.0;
        vec2 uvDx = vec2(0.0);
        vec2 uvDy = vec2(0.0);
        if (hasTextEffects)
        {
#if defined(GE_COMPAT_PROFILE)
            uvDx = ge_UiDeriv.UvDdx;
            uvDy = ge_UiDeriv.UvDdy;
#else
            uvDx = dFdx(vUV);
            uvDy = dFdy(vUV);
#endif
        }
#if defined(GE_COMPAT_PROFILE)
        uint textureSlot = vTexSlot;
#else
        uint textureSlot = 0u;
#endif

        if (isColorGlyph)
        {
            // Color emoji: the sRGB atlas sample arrives decoded; the adapter
            // moves it into the pipeline's blend space before the tint.
            uint texIdx = prim.packed.y;
            vec4 tex = uiSampleAtlasColor(GE_UI_TEX_LINEAR(texIdx, vTexSlot, vUV));
            vec4 c = tex * glyphColor;
            if (hasTextEffects)
            {
                // The shadow widened the quad past the glyph's atlas rect,
                // where the fill is empty.
                bool insideGlyph = all(greaterThanEqual(vUV, prim.uvRect.xy))
                                && all(lessThanEqual(vUV, prim.uvRect.zw));
                float k = clipA * opacity;
                vec4 layer = textEffectLayer(prim, textureSlot, vUV, uvDx, uvDy, 1.0);
                emitPremultiplied(layer * (k * textEffectUnderFill(insideGlyph ? c.a : 0.0, k)));
                return;
            }
            c.a *= clipA * opacity;
            emitPremultiplied(toPremultipliedSdrUi(c));
        }
        else
        {
            // Slug: evaluate Bezier curves per-pixel
            // prim.radii = bandTransform (scaleX, scaleY, offsetX, offsetY)
            // prim.borderWidths = glyph data packed as intBitsToFloat
            // prim.packed.y = curve texture index, prim.packed.z = band texture index
            vec2 emCoord = vUV;
            vec4 bandTransform = prim.radii;
            ivec4 glyphData = ivec4(floatBitsToInt(prim.borderWidths.x),
                                    floatBitsToInt(prim.borderWidths.y),
                                    floatBitsToInt(prim.borderWidths.z),
                                    floatBitsToInt(prim.borderWidths.w));

            float coverage = GE_UI_SLUG(prim.packed.y, prim.packed.z, vTexSlot,
                                        emCoord, bandTransform, glyphData);
            vec4 effectLayer = hasTextEffects
                ? textEffectLayer(prim, textureSlot, emCoord, uvDx, uvDy, coverage)
                : vec4(0.0);

#ifdef UI_SUBPIXEL_DUAL_SRC
            // Subpixel RGB AA: evaluate coverage again at the red and blue
            // stripe centres, ±1/3 pixel horizontally (RGB-stripe panel,
            // R leftmost). The step rides the screen-x derivative of the
            // em-space interpolant, so it is exact at any glyph scale/DPI.
            // Slug's per-tap footprint (the 1-px pixel box convolved with
            // the kTextFilterWidthPx reconstruction filter) overlaps
            // neighbouring taps, which is the low-pass that keeps fringes
            // bounded.
#if defined(GE_COMPAT_PROFILE)
            vec2 subpixelStepEm = ge_UiDeriv.UvDdx * (1.0 / 3.0);
#else
            vec2 subpixelStepEm = dFdx(emCoord) * (1.0 / 3.0);
#endif
            vec3 coverageRGB = vec3(
                GE_UI_SLUG(prim.packed.y, prim.packed.z, vTexSlot,
                           emCoord - subpixelStepEm, bandTransform, glyphData),
                coverage,
                GE_UI_SLUG(prim.packed.y, prim.packed.z, vTexSlot,
                           emCoord + subpixelStepEm, bandTransform, glyphData));

            // Same colour-keyed correction as the grayscale path, per channel
            // with a shared luminance key (see correctTextCoverageRGB).
            coverageRGB = correctTextCoverageRGB(coverageRGB, glyphColor.rgb,
                                                 pc.textContrast, pc.textBlendGamma,
                                                 pc.textBlendSpaceEncoded);
            coverageRGB = vec3(applyEdgeSoftness(coverageRGB.r),
                               applyEdgeSoftness(coverageRGB.g),
                               applyEdgeSoftness(coverageRGB.b));

            vec3 alphaRGB = glyphColor.a * coverageRGB * (clipA * opacity);
            // src0 = colour premultiplied per channel; src1 = per-channel
            // alpha. dst' = src0 + dst * (1 - src1) then blends each channel
            // by its own stripe coverage. The scalar alpha keeps the mean so
            // destination-alpha accumulation matches the grayscale path's ink.
            // Subpixel is gated to SDR output, where encodeSdrUiForOutput is
            // identity in both blend-space variants; it is kept here so the
            // paint path stays uniform.
            vec3 glyphPaint = encodeSdrUiForOutput(glyphColor.rgb);
            float alphaMean = dot(alphaRGB, vec3(1.0 / 3.0));
            outColor = vec4(glyphPaint * alphaRGB, alphaMean);
            outCoverage = vec4(alphaRGB, alphaMean);
            if (hasTextEffects)
            {
                // The effect instance, weighted per channel so the fill's own
                // per-channel blend over it leaves the layer under the fill.
                float k = clipA * opacity;
                vec3 weight = k * textEffectUnderFill(glyphColor.a * coverageRGB, k);
                vec3 layerA = effectLayer.a * weight;
                float layerMean = dot(layerA, vec3(1.0 / 3.0));
                outColor = vec4(effectLayer.rgb * weight, layerMean);
                outCoverage = vec4(layerA, layerMean);
            }
#else
            // Colour-keyed coverage correction (Skia's SkMaskGamma model, see
            // UI/text_mask_gamma.glsl). This replaced a ppem-keyed
            // pow(coverage, e) ramp, which took up to a third of the ink out of
            // a glyph purely for being asked for at a larger size and thinned
            // both contrast polarities. The perceptual error that needs
            // correcting is a property of the text colour, not of its size.
            coverage = correctTextCoverage(coverage, glyphColor.rgb,
                                           pc.textContrast, pc.textBlendGamma,
                                           pc.textBlendSpaceEncoded);

            coverage = applyEdgeSoftness(coverage);

            if (hasTextEffects)
            {
                float k = clipA * opacity;
                emitPremultiplied(effectLayer * (k * textEffectUnderFill(glyphColor.a * coverage, k)));
                return;
            }
            float alpha = glyphColor.a * coverage * clipA * opacity;
            emitPremultiplied(toPremultipliedSdrUi(vec4(glyphColor.rgb, alpha)));
#endif
        }
        return;
    }

    // ---- MODE: Textured ----
    if (mode == MODE_TEXTURED)
    {
        // border-radius clips background imagery: the generator stores the
        // element's border box and radii in otherwise-unused Textured fields
        // when any corner is rounded. Radii stay zero for un-rounded elements
        // and non-background producers, skipping the mask.
        vec4 paintRect = prim.borderWidths;
        float halfW = max(0.0, paintRect.z) * 0.5;
        float halfH = max(0.0, paintRect.w) * 0.5;
        vec4 paintRadX = clamp(prim.radii, vec4(0.0), vec4(halfW));
        vec4 paintRadY = clamp(prim.radiiY, vec4(0.0), vec4(halfH));
        float roundedA = 1.0;
        if (max(max(paintRadX.x, paintRadX.y), max(paintRadX.z, paintRadX.w)) > 0.0)
        {
            roundedA = sdfCoverage(roundedRectSDF(p, paintRect, paintRadX, paintRadY), px);
            if (roundedA <= 0.0)
                discard;
        }

        uint texIdx = prim.packed.y;
        vec4 tint = uiPaintColor(prim.colors.x);
        // modeParams.y > 0.5 signals point (nearest-neighbor) filtering.
        vec4 tex;
        if (prim.modeParams.y > 0.5)
            tex = GE_UI_TEX_NEAREST(texIdx, vTexSlot, vUV);
        else
            tex = GE_UI_TEX_LINEAR(texIdx, vTexSlot, vUV);
        bool hdrTexture = (flags & HDR_TEXTURE_BIT) != 0u;
        tex = uiSampleExternal(tex, flags);
        // Optional desaturation of the source sample (CSS background-image-saturation).
        // modeParams.w = saturation (1 = full color, 0 = grayscale luma).
        float sat = clamp(prim.modeParams.w, 0.0, 1.0);
        if (sat < 1.0)
        {
            float luma = dot(tex.rgb, vec3(0.2126, 0.7152, 0.0722));
            tex.rgb = mix(vec3(luma), tex.rgb, sat);
        }
        vec4 c = tex * tint;
        c.a *= roundedA * clipA * opacity;
        emitPremultiplied(hdrTexture ? vec4(c.rgb * c.a, c.a) : toPremultipliedSdrUi(c));
        return;
    }

    // ---- MODE: Line ----
    // Endpoints are in uvRect (rect is the expanded bounding box for rasterization).
    if (mode == MODE_LINE)
    {
        vec2 a = prim.uvRect.xy;
        vec2 b = prim.uvRect.zw;
        float halfThick = prim.radii.x * 0.5;

        vec2 ab = b - a;
        float len2 = dot(ab, ab);
        float sd;

        if ((flags & SQUARE_CAP_BIT) != 0u)
        {
            float len = sqrt(max(len2, 0.0001));
            vec2 dir = ab / len;
            vec2 perp = vec2(-dir.y, dir.x);
            float projLen = dot(p - a, dir);
            float dCap = max(-projLen, projLen - len);
            float dPerp = abs(dot(p - a, perp)) - halfThick;
            sd = max(dCap, dPerp);
        }
        else
        {
            float t = clamp(dot(p - a, ab) / max(len2, 0.0001), 0.0, 1.0);
            sd = length(p - a - ab * t) - halfThick;
        }

        // Same footprint-based box coverage the rect path uses. A stroke's
        // field has a valley along its centreline, so fwidth(sd) collapses
        // wherever a 2x2 quad straddles it — which made a dashed border's
        // horizontal and vertical runs render at different brightness.
        float alpha = sdfCoverage(-sd, px);

        vec4 lineColor = uiPaintColor(prim.colors.x);
        emitPremultiplied(toPremultipliedSdrUi(vec4(lineColor.rgb, lineColor.a * alpha * clipA * opacity)));
        return;
    }

    // ---- MODE: Bezier ----
    // Endpoints P0,P3 in uvRect; control points P1,P2 in borderWidths.
    // Rect is the expanded bounding box for rasterization.
    if (mode == MODE_BEZIER)
    {
        vec2 bp0 = prim.uvRect.xy;
        vec2 bp3 = prim.uvRect.zw;
        vec2 bp1 = prim.borderWidths.xy;
        vec2 bp2 = prim.borderWidths.zw;
        float halfThick = prim.radii.x * 0.5;

        float dist = cubicBezierDist(p, bp0, bp1, bp2, bp3);
        float sd = dist - halfThick;

        if ((flags & SQUARE_CAP_BIT) != 0u)
        {
            // Flatten the round end caps to planes perpendicular to the
            // endpoint tangents. Chained segments/arcs that share an endpoint
            // and tangent then abut exactly, with no overlapping round caps
            // (whose blended AA fringes otherwise bump the seam).
            vec2 t0 = bp1 - bp0;
            if (dot(t0, t0) < 1e-12) t0 = bp3 - bp0;
            vec2 t1 = bp3 - bp2;
            if (dot(t1, t1) < 1e-12) t1 = bp3 - bp0;
            t0 = normalize(t0);
            t1 = normalize(t1);
            float dStart = dot(p - bp0, -t0);
            float dEnd   = dot(p - bp3,  t1);
            sd = max(sd, max(dStart, dEnd));
        }

        // Same footprint-based box coverage the stroke path uses. A curve's
        // distance field has a valley along its centreline, so fwidth(sd)
        // collapses wherever a 2x2 quad straddles it and the AA width becomes
        // a function of where the quad grid falls — a graph connection's
        // straight legs changed brightness as they moved by whole pixels.
        float alpha = sdfCoverage(-sd, px);

        vec4 curveColor = uiPaintColor(prim.colors.x);
        emitPremultiplied(toPremultipliedSdrUi(vec4(curveColor.rgb, curveColor.a * alpha * clipA * opacity)));
        return;
    }

    // ---- MODE: Triangle ----
    // Three vertices: v0 in uvRect.xy, v1 in uvRect.zw, v2 in borderWidths.xy.
    // Uses cross-product edge tests for a smooth SDF-based fill.
    if (mode == MODE_TRIANGLE)
    {
        vec2 v0 = prim.uvRect.xy;
        vec2 v1 = prim.uvRect.zw;
        vec2 v2 = prim.borderWidths.xy;

        // Signed distances to each edge (positive = inside when winding is CCW).
        // Ensure consistent winding: if the triangle is CW, flip all signs.
        vec2 e0 = v1 - v0;
        vec2 e1 = v2 - v1;
        vec2 e2 = v0 - v2;

        float d0 = (p.x - v0.x) * e0.y - (p.y - v0.y) * e0.x;
        float d1 = (p.x - v1.x) * e1.y - (p.y - v1.y) * e1.x;
        float d2 = (p.x - v2.x) * e2.y - (p.y - v2.y) * e2.x;

        // Normalise to pixel-space signed distance.
        float len0 = max(length(e0), 0.001);
        float len1 = max(length(e1), 0.001);
        float len2 = max(length(e2), 0.001);
        d0 /= len0;
        d1 /= len1;
        d2 /= len2;

        // Handle CW winding by flipping signs.
        float cross = e0.x * (v2.y - v0.y) - e0.y * (v2.x - v0.x);
        float sign = cross >= 0.0 ? 1.0 : -1.0;
        d0 *= sign;
        d1 *= sign;
        d2 *= sign;

        float sd = max(max(d0, d1), d2); // all negative = inside; any positive = outside
        float rounding = prim.radii.x;
        sd += rounding; // shrink inward, then expand by rounding (Minkowski with circle)
        // Box coverage from the pixel footprint, as the rect and stroke paths
        // use. fwidth is the L1 norm |dFdx|+|dFdy|, which runs from 1 on an
        // axis-aligned edge to sqrt(2) on a diagonal, so the AA band widened
        // by 41% along an arrow's slanted edges; and it reads the creases
        // where the max() above switches edges, which put the corner pixels'
        // coverage at the mercy of where the quad grid falls.
        float alpha = sdfCoverage(-sd, px);

        vec4 triColor = uiPaintColor(prim.colors.x);
        emitPremultiplied(toPremultipliedSdrUi(vec4(triColor.rgb, triColor.a * alpha * clipA * opacity)));
        return;
    }

    discard;
}
