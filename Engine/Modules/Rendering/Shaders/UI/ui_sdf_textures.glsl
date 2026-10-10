// UI SDF texture access, set 1 — the single declaration site for the UI
// renderer's sampled images. The twin of Includes/bindless_textures.glsl:
// same two-profile shape, same reason.
//
// Full profile — runtime-sized bindless arrays indexed per primitive by the
// UITextureRegistry index the generator stored in packed.y / packed.z.
//
// Compat profile — one fixed binding per slot, filled from a per-run bind
// group. WebGPU has no binding arrays at ANY size (Chrome refuses even a
// bounded one: "use of 'binding_array' requires the 'sized_binding_array'
// language feature"), so the slot ordinal resolves in a switch and the CPU
// partitions the draw order into runs whose distinct textures fit the slots
// (UICompatDrawRuns). Slug's curve and band textures are ONE slot family, not
// two: a glyph always needs both together, so pairing them keeps the text path
// at one switch of GE_UI_COMPAT_GLYPH_SLOTS arms instead of a product of two.
#ifndef UI_SDF_TEXTURES_GLSL
#define UI_SDF_TEXTURES_GLSL

#if defined(GE_COMPAT_PROFILE)

// Must match UI::kUiCompatTextureSlots / kUiCompatGlyphSlots and the layout
// UITextureRegistry builds under the compat profile. The two families plus the
// paired band images total 16 sampled textures in the fragment stage — core
// WebGPU's maxSampledTexturesPerShaderStage.
#define GE_UI_COMPAT_TEXTURE_SLOTS 8
#define GE_UI_COMPAT_GLYPH_SLOTS   4

layout(set = 1, binding = 0) uniform texture2D ge_UiTexture0;
layout(set = 1, binding = 1) uniform texture2D ge_UiTexture1;
layout(set = 1, binding = 2) uniform texture2D ge_UiTexture2;
layout(set = 1, binding = 3) uniform texture2D ge_UiTexture3;
layout(set = 1, binding = 4) uniform texture2D ge_UiTexture4;
layout(set = 1, binding = 5) uniform texture2D ge_UiTexture5;
layout(set = 1, binding = 6) uniform texture2D ge_UiTexture6;
layout(set = 1, binding = 7) uniform texture2D ge_UiTexture7;

layout(set = 1, binding = 8)  uniform texture2D ge_UiGlyphCurve0;
layout(set = 1, binding = 9)  uniform texture2D ge_UiGlyphCurve1;
layout(set = 1, binding = 10) uniform texture2D ge_UiGlyphCurve2;
layout(set = 1, binding = 11) uniform texture2D ge_UiGlyphCurve3;

layout(set = 1, binding = 12) uniform utexture2D ge_UiGlyphBand0;
layout(set = 1, binding = 13) uniform utexture2D ge_UiGlyphBand1;
layout(set = 1, binding = 14) uniform utexture2D ge_UiGlyphBand2;
layout(set = 1, binding = 15) uniform utexture2D ge_UiGlyphBand3;

layout(set = 1, binding = 16) uniform sampler uiLinearSampler;
layout(set = 1, binding = 17) uniform sampler uiNearestSampler;

// GLSL cannot return an opaque sampler type from a function, so each slot walk
// has to complete the operation it selects for.
vec4 uiSampleTextureSlot(uint slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    switch (slot)
    {
    case 0u: return textureGrad(sampler2D(ge_UiTexture0, uiLinearSampler), uv, ddx, ddy);
    case 1u: return textureGrad(sampler2D(ge_UiTexture1, uiLinearSampler), uv, ddx, ddy);
    case 2u: return textureGrad(sampler2D(ge_UiTexture2, uiLinearSampler), uv, ddx, ddy);
    case 3u: return textureGrad(sampler2D(ge_UiTexture3, uiLinearSampler), uv, ddx, ddy);
    case 4u: return textureGrad(sampler2D(ge_UiTexture4, uiLinearSampler), uv, ddx, ddy);
    case 5u: return textureGrad(sampler2D(ge_UiTexture5, uiLinearSampler), uv, ddx, ddy);
    case 6u: return textureGrad(sampler2D(ge_UiTexture6, uiLinearSampler), uv, ddx, ddy);
    default: return textureGrad(sampler2D(ge_UiTexture7, uiLinearSampler), uv, ddx, ddy);
    }
}

vec4 uiSampleTextureSlotNearest(uint slot, vec2 uv, vec2 ddx, vec2 ddy)
{
    switch (slot)
    {
    case 0u: return textureGrad(sampler2D(ge_UiTexture0, uiNearestSampler), uv, ddx, ddy);
    case 1u: return textureGrad(sampler2D(ge_UiTexture1, uiNearestSampler), uv, ddx, ddy);
    case 2u: return textureGrad(sampler2D(ge_UiTexture2, uiNearestSampler), uv, ddx, ddy);
    case 3u: return textureGrad(sampler2D(ge_UiTexture3, uiNearestSampler), uv, ddx, ddy);
    case 4u: return textureGrad(sampler2D(ge_UiTexture4, uiNearestSampler), uv, ddx, ddy);
    case 5u: return textureGrad(sampler2D(ge_UiTexture5, uiNearestSampler), uv, ddx, ddy);
    case 6u: return textureGrad(sampler2D(ge_UiTexture6, uiNearestSampler), uv, ddx, ddy);
    default: return textureGrad(sampler2D(ge_UiTexture7, uiNearestSampler), uv, ddx, ddy);
    }
}

// Sampling entry points. Both operands are always passed: the full profile uses
// the registry index and drops the slot, the compat profile the reverse. The
// unused argument never reaches the GLSL parser, so a compat-only varying costs
// the full-profile shader nothing.
#define GE_UI_TEX_LINEAR(texIdx, texSlot, uv) \
    uiSampleTextureSlot(texSlot, uv, ge_UiDeriv.UvDdx, ge_UiDeriv.UvDdy)
#define GE_UI_TEX_NEAREST(texIdx, texSlot, uv) \
    uiSampleTextureSlotNearest(texSlot, uv, ge_UiDeriv.UvDdx, ge_UiDeriv.UvDdy)
#define GE_UI_SLUG(curveIdx, bandIdx, texSlot, emCoord, bandTransform, glyphData) \
    uiSlugRenderSlot(texSlot, emCoord, bandTransform, glyphData)

// Requires slug_functions.glsl (via sdf_functions.glsl) to be included first:
// this completes the Slug evaluation for the pair the ordinal selects, so the
// text path branches once on the slot rather than once per texture operand.
float uiSlugRenderSlot(uint slot, vec2 emCoord, vec4 bandTransform, ivec4 glyphData)
{
    switch (slot)
    {
    case 0u:
        return slugRender(ge_UiGlyphCurve0, ge_UiGlyphBand0, uiLinearSampler,
                          uiNearestSampler, emCoord, bandTransform, glyphData);
    case 1u:
        return slugRender(ge_UiGlyphCurve1, ge_UiGlyphBand1, uiLinearSampler,
                          uiNearestSampler, emCoord, bandTransform, glyphData);
    case 2u:
        return slugRender(ge_UiGlyphCurve2, ge_UiGlyphBand2, uiLinearSampler,
                          uiNearestSampler, emCoord, bandTransform, glyphData);
    default:
        return slugRender(ge_UiGlyphCurve3, ge_UiGlyphBand3, uiLinearSampler,
                          uiNearestSampler, emCoord, bandTransform, glyphData);
    }
}

#define GE_UI_SLUG_DISTANCE(curveIdx, bandIdx, texSlot, emCoord, bandTransform, glyphData, emsPerPixel, reachPx) \
    uiSlugDistanceSlot(texSlot, emCoord, bandTransform, glyphData, emsPerPixel, reachPx)

// The outline distance for the pair the ordinal selects (text effects).
float uiSlugDistanceSlot(uint slot, vec2 emCoord, vec4 bandTransform, ivec4 glyphData,
                         vec2 emsPerPixel, float reachPx)
{
    switch (slot)
    {
    case 0u:
        return slugOutlineDistance(ge_UiGlyphCurve0, ge_UiGlyphBand0, uiLinearSampler, uiNearestSampler,
                                   emCoord, bandTransform, glyphData, emsPerPixel, reachPx);
    case 1u:
        return slugOutlineDistance(ge_UiGlyphCurve1, ge_UiGlyphBand1, uiLinearSampler, uiNearestSampler,
                                   emCoord, bandTransform, glyphData, emsPerPixel, reachPx);
    case 2u:
        return slugOutlineDistance(ge_UiGlyphCurve2, ge_UiGlyphBand2, uiLinearSampler, uiNearestSampler,
                                   emCoord, bandTransform, glyphData, emsPerPixel, reachPx);
    default:
        return slugOutlineDistance(ge_UiGlyphCurve3, ge_UiGlyphBand3, uiLinearSampler, uiNearestSampler,
                                   emCoord, bandTransform, glyphData, emsPerPixel, reachPx);
    }
}

#else // full profile

layout(set = 1, binding = 0) uniform texture2D textures[];
layout(set = 1, binding = 1) uniform utexture2D bandTextures[];
layout(set = 1, binding = 2) uniform sampler uiLinearSampler;
layout(set = 1, binding = 3) uniform sampler uiNearestSampler;

#define GE_UI_TEX_LINEAR(texIdx, texSlot, uv) \
    texture(sampler2D(textures[nonuniformEXT(texIdx)], uiLinearSampler), uv)
#define GE_UI_TEX_NEAREST(texIdx, texSlot, uv) \
    texture(sampler2D(textures[nonuniformEXT(texIdx)], uiNearestSampler), uv)
#define GE_UI_SLUG(curveIdx, bandIdx, texSlot, emCoord, bandTransform, glyphData) \
    slugRender(textures[nonuniformEXT(curveIdx)], bandTextures[nonuniformEXT(bandIdx)], \
               uiLinearSampler, uiNearestSampler, emCoord, bandTransform, glyphData)

#define GE_UI_SLUG_DISTANCE(curveIdx, bandIdx, texSlot, emCoord, bandTransform, glyphData, emsPerPixel, reachPx) \
    slugOutlineDistance(textures[nonuniformEXT(curveIdx)], bandTextures[nonuniformEXT(bandIdx)], \
                        uiLinearSampler, uiNearestSampler, emCoord, bandTransform, glyphData, emsPerPixel, reachPx)

#endif // GE_COMPAT_PROFILE

#endif // UI_SDF_TEXTURES_GLSL
