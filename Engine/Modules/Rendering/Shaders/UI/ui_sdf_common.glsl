// Shared definitions for the UI SDF instanced renderer.
// Included by both ui_sdf.vert and ui_sdf.frag.

#ifndef UI_SDF_COMMON_GLSL
#define UI_SDF_COMMON_GLSL

// --- Push constants ---
layout(push_constant) uniform PC {
    vec2 targetSize;
    float timeSeconds;
    float textContrast;      // Skia SK_GAMMA_CONTRAST for the colour-keyed coverage boost; 0 = off
    float textBlendGamma;    // target blend space for coverage; 0 = linear (identity retarget)
    float edgeSoftness;      // user softness knob; brightness-neutral remap around 0.5
    int outputEncoding;      // 0=SDR linear, 1=SDR encoded bytes, 2=HDR10 PQ, 3=HLG, 4=scRGB
    float paperWhiteNits;    // UI white TARGET (nits) in HDR output modes
    float blackLiftNits;     // SDR UI shadow offset (nits) in HDR output modes
    float deviceWhiteNits;   // device paper-white (nits) — the intermediate's 1.0 anchor
    // Caret blink phase-toggle rate (toggles/second): the reciprocal of the OS
    // caret blink half-period. 0 = never blink, which pins floor(t * rate) at 0
    // and leaves the caret solid.
    float caretPhaseTogglesPerSecond;
    // 1 when the attachment interpolates raw sRGB-encoded bytes (the browser
    // blend model) — selects the Skia-direction text retarget arm. 0 = the
    // attachment interpolates linear values. See UI/text_mask_gamma.glsl.
    int textBlendSpaceEncoded;
#if defined(GE_COMPAT_PROFILE)
    // First draw-order index of the run this draw covers. The compat profile
    // splits the UI's single instanced draw into per-texture runs; see
    // ui_sdf.vert for why firstInstance cannot carry this.
    uint drawBase;
    // Pads the block to a multiple of its largest alignment (vec2: 8), the
    // size the MSL struct rounds up to, so Metal reads exactly what is pushed.
    uint drawBasePad;
#endif
} pc;

// --- UIPrimitive (144 bytes, 9 x vec4) ---
// A corner is an ellipse: radii holds the horizontal semi-axes and radiiY the
// vertical ones. Modes that overload the radii slot for something else (Line/
// Bezier thickness, Triangle rounding) leave radiiY zero. Slug overloads radii
// with its band transform and keeps its text outline width in radiiY.x.
struct UIPrimitive {
    vec4 rectXYWH;
    vec4 radii;          // Rect: corners; Textured: border-radius background clip corners
    uvec4 colors;        // fillColor, borderColor (Slug: outline), shadowColor, glowColor
    vec4 borderWidths;   // Rect: l/t/r/b; Textured: background clip rect (border box)
    vec4 uvRect;
    vec4 effects;        // shadowOffsetX, shadowOffsetY, shadowSoftness (CSS blur radius), glowRadius
    vec4 modeParams;     // opacity, pointFilter (Textured: 1=nearest 0=linear), caretTime, saturation (Textured)
    uvec4 packed;        // modeAndFlags, textureIndex, gradColor0, gradColor1
    vec4 radiiY;         // Rect/Textured: vertical corner semi-axes (tl, tr, br, bl); Slug: x = outline width
};

// --- UIClipRect (64 bytes, 4 x vec4) ---
// parentIndex links to the enclosing clip so the shader can walk the chain.
struct UIClipRect {
    vec4 rect;       // x, y, w, h
    vec4 radii;      // tl, tr, br, bl horizontal semi-axes
    vec4 radiiY;     // tl, tr, br, bl vertical semi-axes
    uvec4 params;    // .x = parentIndex (NO_CLIP terminates), .yzw = unused
};

// --- Mode/flag bit layout (matches UIPrimitive.h) ---
const uint MODE_MASK        = 0x000000FFu;
const uint GRADIENT_SHIFT   = 8u;
const uint GRADIENT_MASK    = 0x00000700u;
const uint ENCODED_SRC_BIT  = 0x00000800u;
const uint COLOR_GLYPH_BIT  = 0x00001000u;
const uint CARET_BIT        = 0x00002000u;
const uint SQUARE_CAP_BIT   = 0x00004000u;
const uint HDR_TEXTURE_BIT  = 0x00008000u;
const uint CLIP_INDEX_SHIFT = 16u;
const uint NO_CLIP          = 0xFFFFu;

const uint MODE_RECT     = 0u;
const uint MODE_SLUG     = 1u;
const uint MODE_TEXTURED = 2u;
const uint MODE_LINE     = 3u;
const uint MODE_BEZIER   = 4u;
const uint MODE_TRIANGLE = 5u;

const uint GRAD_NONE        = 0u;
const uint GRAD_VERTICAL    = 1u;
const uint GRAD_HORIZONTAL  = 2u;
const uint GRAD_FOUR_CORNER = 3u;
const uint GRAD_POLAR_HSV   = 4u;
const uint GRAD_HUE_VERT    = 5u;
const uint GRAD_POLAR_HSV_GRADING = 6u;

// --- Slug text reconstruction filter ---
// Box kernel convolved onto the exact-area glyph coverage, in DEVICE pixels.
// Matches the filter measured out of Chrome's rasteriser (edge-profile
// decomposition over Chrome vs engine specimen crops): best-fit extra box
// width 0.945 px at BOTH dpr 1 and dpr 2 — fixed in device pixels, not
// CSS pixels. It is mass-preserving (Chrome mask mass / geometric stem width
// = 0.9962), widens the 10-90 edge from 0.83 px (bare exact-area) to Chrome's
// 1.20 px, and damps stem-phase contrast. Shared by the fragment stage (the
// widened coverage ramp in slug_functions.glsl) and the vertex stage (glyph
// quad dilation must cover the widened skirt).
const float kTextFilterWidthPx = 0.945;

// A crossing influences samples up to this far away: the exact-area ramp's
// 0.5 px reach plus half the filter width.
const float kTextFilterOuterRadiusPx = 0.5 + 0.5 * kTextFilterWidthPx;

// --- Text effect reach ---
// How far, in device pixels, a Slug primitive's text effects paint past its
// glyph box (UI/text_effects.glsl evaluates them; the vertex stage widens the
// glyph quad by this much). An outline reaches its width plus the one-pixel
// ramp; a glow kTextGlowReachRadii radii past the outline, where its falloff
// exp(-2 (g / R)^2) is below 1/255; a shadow its offset plus three sigma of
// its blur (sigma = blur / 2) past the outline. Colour glyphs have no outline
// to measure a distance from and take the shadow alone.
//
// radiiY.x is the outline width the shadow and the glow start from; the
// outline itself paints only with a visible colors.y. A run's shadow-and-glow
// instance keeps the width with a transparent outline colour, and its outline
// instance carries the outline alone (GlyphRunEmitter).
const float kTextGlowReachRadii = 2.0;
const float kTextShadowReachBlurs = 1.5;

bool uiTextEffectVisible(uint packedRgba8)
{
    return (packedRgba8 >> 24u) != 0u;
}

float uiTextOutlineWidthPx(UIPrimitive prim)
{
    bool colorGlyph = (prim.packed.x & COLOR_GLYPH_BIT) != 0u;
    return !colorGlyph ? max(prim.radiiY.x, 0.0) : 0.0;
}

bool uiTextOutlinePainted(UIPrimitive prim)
{
    return uiTextOutlineWidthPx(prim) > 0.0 && uiTextEffectVisible(prim.colors.y);
}

float uiTextGlowRadiusPx(UIPrimitive prim)
{
    bool colorGlyph = (prim.packed.x & COLOR_GLYPH_BIT) != 0u;
    return !colorGlyph && uiTextEffectVisible(prim.colors.w) ? max(prim.effects.w, 0.0) : 0.0;
}

bool uiTextShadowActive(UIPrimitive prim)
{
    return uiTextEffectVisible(prim.colors.z)
        && (prim.effects.x != 0.0 || prim.effects.y != 0.0 || prim.effects.z > 0.0);
}

float uiTextEffectReachPx(UIPrimitive prim)
{
    float width = uiTextOutlineWidthPx(prim);
    float glow = uiTextGlowRadiusPx(prim);
    float reach = uiTextOutlinePainted(prim) ? width + 1.0 : 0.0;
    if (glow > 0.0)
        reach = max(reach, width + kTextGlowReachRadii * glow);
    if (uiTextShadowActive(prim))
        reach = max(reach, max(abs(prim.effects.x), abs(prim.effects.y)) + width
                               + kTextShadowReachBlurs * max(prim.effects.z, 0.0) + 1.0);
    return reach;
}

// --- Color unpacking (RGBA8 -> vec4) ---
vec4 unpackRGBA8(uint c)
{
    return vec4(float(c & 0xFFu),
                float((c >> 8u) & 0xFFu),
                float((c >> 16u) & 0xFFu),
                float((c >> 24u) & 0xFFu)) / 255.0;
}

// Canonical HDR paper-white floor (the intermediate's 1.0 == 80 nits anchor).
// Mirrors Rendering::kHdrPaperWhiteFloorNits and the same const in
// encode_srgb.frag / tonemap.frag.
const float kHdrPaperWhiteFloorNits = 80.0;

// --- Source-space adapters ---
// The blend space is a property of the ATTACHMENT, fixed per pipeline
// variant: linear light by default, raw sRGB-encoded bytes when compiled
// with UI_BLEND_SPACE_ENCODED (the EncodedSrgb target — the browser/Skia
// compositing model, #767). These adapters are the ONLY entry points for
// colour into the blend space; there is deliberately no free-standing sRGB
// transfer pair here, so a new colour source that skips its adapter fails
// to compile instead of blending in the wrong space. (Text coverage
// retargeting in text_mask_gamma.glsl keeps its own scalar transfer pair:
// it transforms COVERAGE between blend models, not colour into the blend
// space.)

#ifdef UI_BLEND_SPACE_ENCODED

// Encoded blend space. sRGB OETF for sources that arrive linear (sampled
// textures); CSS paint needs no transfer at all — the bytes ARE the blend
// values, which is what deletes the pow chains from the paint path.
vec3 uiEncodeSrgb(vec3 linearRgb)
{
    vec3 c = clamp(linearRgb, vec3(0.0), vec3(1.0));
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055;
    return mix(lo, hi, greaterThan(c, vec3(0.0031308)));
}

// CSS-authored paint (sRGB) -> blend space: identity. Float form for
// procedurally computed sRGB colour (the HSV colour-picker wheels); alpha
// is coverage, passed through untouched.
vec4 uiPaintColor(vec4 srgb)
{
    return srgb;
}

// CSS-authored paint, packed RGBA8 bytes (sRGB) -> blend space: the raw bytes.
vec4 uiPaintColor(uint packedRgba8)
{
    return unpackRGBA8(packedRgba8);
}

// External-texture sample -> blend space. The sampler hands SDR content over
// decoded (sRGB view) and HDR content linear; the encoded target is SDR-only,
// so both clamp to SDR range and take the OETF at sample time. HDR highlights
// above 1.0 clip here — an EncodedSrgb attachment cannot represent them.
//
// ENCODED_SRC_BIT is the identity case: a world view that finalized itself
// already applied this OETF and quantized to the presented step, and its texels
// arrive undecoded (plain view, no sRGB transfer). Encoding again would apply
// the curve twice and darken the whole viewport; leaving the values alone is
// also what keeps them byte-exact all the way to the backbuffer.
vec4 uiSampleExternal(vec4 texel, uint primFlags)
{
    if ((primFlags & ENCODED_SRC_BIT) != 0u)
        return texel;
    texel.rgb = uiEncodeSrgb(texel.rgb);
    return texel;
}

// Colour-glyph atlas sample (emoji) -> blend space. The atlas view is sRGB,
// so the texel arrives decoded; the encoded target re-encodes at sample time.
vec4 uiSampleAtlasColor(vec4 texel)
{
    return vec4(uiEncodeSrgb(texel.rgb), texel.a);
}

#else // linear blend space

// CSS-authored paint (sRGB) -> blend space. Float form for procedurally
// computed sRGB colour (the HSV colour-picker wheels); alpha is coverage,
// passed through untouched.
vec4 uiPaintColor(vec4 srgb)
{
    vec3 lo = srgb.rgb / 12.92;
    vec3 hi = pow((srgb.rgb + 0.055) / 1.055, vec3(2.4));
    vec3 linear = mix(lo, hi, greaterThan(srgb.rgb, vec3(0.04045)));
    return vec4(linear, srgb.a);
}

// CSS-authored paint, packed RGBA8 bytes (sRGB) -> blend space.
vec4 uiPaintColor(uint packedRgba8)
{
    return uiPaintColor(unpackRGBA8(packedRgba8));
}

// External-texture sample -> blend space, keyed on the primitive's space
// bits. SDR content (HDR_TEXTURE_BIT clear) arrives sampler-decoded to
// linear and is clamped to SDR range; HDR content passes through unbounded.
vec4 uiSampleExternal(vec4 texel, uint primFlags)
{
    if ((primFlags & HDR_TEXTURE_BIT) == 0u)
        texel.rgb = clamp(texel.rgb, vec3(0.0), vec3(1.0));
    return texel;
}

// Colour-glyph atlas sample (emoji) -> blend space: already sampler-decoded
// to linear, which IS the blend space.
vec4 uiSampleAtlasColor(vec4 texel)
{
    return texel;
}

#endif // UI_BLEND_SPACE_ENCODED

// Straight-alpha to premultiplied form.
vec4 toPremultiplied(vec4 c)
{
    vec4 pm = vec4(c.rgb * c.a, c.a);
    pm.rgb = clamp(pm.rgb, vec3(0.0), vec3(pm.a));
    return pm;
}

vec3 liftSdrUiForHdr(vec3 linearRgb, float white, float blackLift)
{
    vec3 rgb = max(linearRgb, vec3(0.0));
    return max(rgb * white + vec3(blackLift), vec3(0.0));
}

#ifdef UI_BLEND_SPACE_ENCODED
// The encoded target is SDR-only by construction (UITargetSpace::EncodedSrgb
// has no HDR flavour), so the HDR paper-white lift is unreachable and this
// compiles to the identity: the blend values already carry the output curve.
vec3 encodeSdrUiForOutput(vec3 rgb)
{
    return rgb;
}
#else
// Producers write scene-referred LINEAR (Rec709, 1.0 == paper-white) into the
// presentation intermediate; the terminal FinalSRGBEncode pass is the sole owner
// of the output transfer function (sRGB / PQ / HLG / scRGB). So in HDR, lift the
// SDR UI to paper-white-relative linear here (no OETF, no BT2020); in SDR it is
// already linear. This matches how HDR-content textures are composited (straight
// linear), so exactly one encoder runs downstream.
vec3 encodeSdrUiForOutput(vec3 linearRgb)
{
    if (pc.outputEncoding >= 2) // any HDR output mode (PQ / HLG / scRGB)
    {
        // UI white renders at its own absolute target (uiWhite nits), normalized by
        // the device paper-white (the intermediate's 1.0 anchor) so the knob controls
        // UI brightness independent of the scene's paper-white. The two floors are
        // distinct quantities: uiWhite >= 40 (min legible UI white), deviceWhite >=
        // kHdrPaperWhiteFloorNits (canonical 80-nit anchor) — so there is no
        // numerator/denominator mismatch.
        float uiWhite = max(pc.paperWhiteNits, 40.0);
        float deviceWhite = max(pc.deviceWhiteNits, kHdrPaperWhiteFloorNits);
        float blackLift = clamp(pc.blackLiftNits, 0.0, 48.0);
        return liftSdrUiForHdr(linearRgb, uiWhite, blackLift) / deviceWhite;
    }
    return linearRgb;
}
#endif // UI_BLEND_SPACE_ENCODED

vec4 encodeSdrUiColor(vec4 linearColor)
{
    return vec4(encodeSdrUiForOutput(linearColor.rgb), linearColor.a);
}

vec4 toPremultipliedSdrUi(vec4 c)
{
    return toPremultiplied(encodeSdrUiColor(c));
}

#endif // UI_SDF_COMMON_GLSL
