// Text shadow, glow and outline of a glyph's effect instance: a Slug primitive
// with effect fields draws this layer and not its fill, and the run emits it
// before every fill of the run (GlyphRunEmitter): every shadow and glow, then
// every outline, so each effect sits under all of the run's fills and every
// glow under every outline. Requires ui_sdf_common.glsl, sdf_functions.glsl and
// ui_sdf_textures.glsl to be included first.
//
// All three are functions of one quantity: the distance from the fragment to
// the glyph outline, which slugOutlineDistance measures from the same curve and
// band data the fill's coverage reads.
//   outline  a second threshold: covered where the distance is within the width,
//            with the one-pixel ramp the fill's own edge has.
//   glow     a wider falloff: exp(-2 (g / R)^2) of the distance g past the
//            outline, the falloff the rect glow uses.
//   shadow   an offset sample: the same evaluation at the fragment minus the
//            offset. Sharp, it is the fill coverage there; blurred, the Gaussian
//            soft edge (sigma = blur / 2, the CSS blur radius) of the signed
//            distance there.
// The shadow and the glow are cast by the outlined glyph. Colour glyphs have no
// outline to measure: they take the shadow alone, as their offset alpha.
//
// Primitive fields (Slug mode, device pixels): colors.y outline colour, colors.z
// shadow colour, colors.w glow colour; effects = (shadow offset x, shadow offset
// y, shadow blur, glow radius); radiiY.x outline width. uiTextEffectReachPx
// (ui_sdf_common.glsl) sizes the quad to match.
#ifndef TEXT_EFFECTS_GLSL
#define TEXT_EFFECTS_GLSL

// The fragment's footprint as a Gaussian of sigma 0.5 px, the pixel term
// gaussianShadowAlphaIntegrated adds for a rect shadow. Fixed here because the
// distance is in device pixels by construction.
const float kTextShadowPixelSigma = 0.5;

// Premultiplied, output-encoded paint of `color` at coverage `coverage`.
vec4 textEffectPaint(uint packedColor, float coverage)
{
    vec4 color = uiPaintColor(packedColor);
    return toPremultipliedSdrUi(vec4(color.rgb, color.a * coverage));
}

vec4 textEffectOver(vec4 top, vec4 bottom)
{
    return top + bottom * (1.0 - top.a);
}

// Coverage of the shadow edge at signed distance `inside` (positive inside the
// shadow's shape), device pixels.
float textShadowSoftEdge(float inside, float blur)
{
    float sigma = 0.5 * blur;
    float sigmaEff = sqrt(sigma * sigma + kTextShadowPixelSigma * kTextShadowPixelSigma);
    return 0.5 + 0.5 * erfApprox(inside / (sigmaEff * 1.4142135));
}

// Alpha of a colour glyph's atlas at uv; zero outside the glyph's own rect,
// which the widened quad extrapolates past.
float textColorGlyphAlpha(UIPrimitive prim, uint textureSlot, vec2 uv)
{
    if (any(lessThan(uv, prim.uvRect.xy)) || any(greaterThan(uv, prim.uvRect.zw)))
        return 0.0;
    return GE_UI_TEX_LINEAR(prim.packed.y, textureSlot, uv).a;
}

// Coverage of the shadow at the fragment: the glyph, dilated by the outline
// width, sampled at the fragment minus the offset.
float textShadowCoverage(UIPrimitive prim, uint textureSlot, vec2 uv, vec2 dx, vec2 dy,
                         vec2 emsPerPixel, float outlineWidth)
{
    vec2 shadowUV = uv - prim.effects.x * dx - prim.effects.y * dy;
    if ((prim.packed.x & COLOR_GLYPH_BIT) != 0u)
        return textColorGlyphAlpha(prim, textureSlot, shadowUV);

    ivec4 glyphData = floatBitsToInt(prim.borderWidths);
    float fill = GE_UI_SLUG(prim.packed.y, prim.packed.z, textureSlot,
                            shadowUV, prim.radii, glyphData);
    float blur = max(prim.effects.z, 0.0);
    if (outlineWidth <= 0.0 && blur <= 0.0)
        return fill;

    float reach = outlineWidth + kTextShadowReachBlurs * blur + 1.0;
    float distance = GE_UI_SLUG_DISTANCE(prim.packed.y, prim.packed.z, textureSlot,
                                         shadowUV, prim.radii, glyphData, emsPerPixel, reach);
    float inside = (fill >= 0.5 ? distance : -distance) + outlineWidth;
    return blur > 0.0 ? textShadowSoftEdge(inside, blur) : clamp(inside + 0.5, 0.0, 1.0);
}

// The effect layer, premultiplied and output-encoded, before clip, opacity and
// textEffectUnderFill. `fillCoverage` is the glyph's raw coverage at the
// fragment (1 for a colour glyph's texel), which signs the distance there.
vec4 textEffectLayer(UIPrimitive prim, uint textureSlot, vec2 uv, vec2 dx, vec2 dy,
                     float fillCoverage)
{
    vec2 emsPerPixel = vec2(length(vec2(dx.x, dy.x)), length(vec2(dx.y, dy.y)));
    float outlineWidth = uiTextOutlineWidthPx(prim);
    float glowRadius = uiTextGlowRadiusPx(prim);

    vec4 layer = vec4(0.0);
    if (uiTextShadowActive(prim))
        layer = textEffectPaint(prim.colors.z,
            textShadowCoverage(prim, textureSlot, uv, dx, dy, emsPerPixel, outlineWidth));

    // A shadow-only instance keeps the outline width it is cast from, but
    // needs no distance at the fragment itself.
    if (glowRadius <= 0.0 && !uiTextOutlinePainted(prim))
        return layer;

    // Outside distance: zero inside the glyph, where the fill covers the layer.
    float reach = max(outlineWidth + 1.0, outlineWidth + kTextGlowReachRadii * glowRadius);
    float distance = 0.0;
    if (fillCoverage < 0.5)
    {
        ivec4 glyphData = floatBitsToInt(prim.borderWidths);
        distance = GE_UI_SLUG_DISTANCE(prim.packed.y, prim.packed.z, textureSlot,
                                       uv, prim.radii, glyphData, emsPerPixel, reach);
    }
    if (glowRadius > 0.0)
    {
        float past = max(distance - outlineWidth, 0.0) / glowRadius;
        layer = textEffectOver(textEffectPaint(prim.colors.w, exp(-2.0 * past * past)), layer);
    }
    if (uiTextOutlinePainted(prim))
        layer = textEffectOver(textEffectPaint(prim.colors.y,
                                               clamp(outlineWidth + 0.5 - distance, 0.0, 1.0)),
                               layer);
    return layer;
}

// Weight of the effect layer such that, once the glyph's fill instance (alpha
// fillA at weight k = clip * opacity) is blended over it, the two draws equal
// k * (fill + (1 - fillA) * layer) over the destination: the layer under the
// fill as one group. (1 - fillA) / (1 - k * fillA): 1 outside the glyph, 0
// under a fully covered fill at any opacity, and 1 at opacity 1 wherever the
// fill is not opaque. The vec3 form is per subpixel channel.
float textEffectUnderFill(float fillA, float k)
{
    return (1.0 - fillA) / max(1.0 - k * fillA, 1e-6);
}

vec3 textEffectUnderFill(vec3 fillA, float k)
{
    return (vec3(1.0) - fillA) / max(vec3(1.0) - k * fillA, vec3(1e-6));
}

#endif // TEXT_EFFECTS_GLSL
