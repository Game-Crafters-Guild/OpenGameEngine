#version 450

// Unified UI SDF instanced vertex shader.
// Generates a quad per instance from UIPrimitive SSBO data.
// One draw call: Draw(6, primitiveCount).

#include "UI/ui_sdf_common.glsl"

layout(std430, set = 0, binding = 0) readonly buffer PrimBuf {
    UIPrimitive prims[];
} sb;

// Stage 2 part 3: DrawOrder indirection. Each draw instance reads its
// primitive slot from drawOrder[gl_InstanceIndex] and then samples that
// slot from the persistent primitive buffer. This decouples slot order
// (allocator-assigned, may have holes) from render order (DFS pre-order
// with z-index priority), so persistent slots can survive across frames
// without disturbing back-to-front draw ordering.
layout(std430, set = 0, binding = 2) readonly buffer DrawOrderBuf {
    uint drawOrder[];
} sb_order;

layout(location = 0) flat out uint vInstance;
layout(location = 1) out vec2 vLocal;  // Pixel position (top-left origin, Y-down)
layout(location = 2) out vec2 vUV;     // Interpolated UV for textured/Slug modes
// Stage 2 part 3: pass the resolved slot index from vertex to fragment so the
// fragment shader reads the SAME primitive the vertex shader used. Without
// this the fragment would re-read sb.prims[vInstance] which bypasses the
// DrawOrder indirection and points at waste / wrong slots in the persistent
// buffer (the cause of the missing-primitive bugs in initial Stage 2 part 3).
layout(location = 3) flat out uint vSlot;
#if defined(GE_COMPAT_PROFILE)
// Run-local texture slot ordinal. The compat profile has no binding arrays, so
// the CPU partitions the draw order into runs whose distinct textures fit the
// fixed slots (UICompatDrawRuns) and packs each primitive's slot ordinal into
// the high bits of its draw-order word. Low 24 bits stay the primitive slot.
layout(location = 4) flat out uint vTexSlot;
const uint UI_DRAW_ORDER_SLOT_MASK = 0x00FFFFFFu;
const uint UI_DRAW_ORDER_TEX_SLOT_SHIFT = 24u;
#endif

// UI geometry is authored in top-left-origin pixels; clip +Y is the TOP row of
// the viewport on every backend the engine targets (Vulkan reaches it with the
// negative viewport height its command list always applies; Metal, D3D12 and
// WebGPU are native there). So the pixel-to-clip mapping negates Y once, for
// all backends, and the pass keeps a plain positive-height viewport rect --
// the only kind WebGPU can express.
vec4 clipFromPixel(vec2 pixel)
{
    vec2 ndc = (pixel / pc.targetSize) * 2.0 - 1.0;
    return vec4(ndc.x, -ndc.y, 0.0, 1.0);
}

vec2 quadPos01(uint vid)
{
    // Two triangles: (0,0)-(1,0)-(1,1) and (0,0)-(1,1)-(0,1)
    if (vid == 0u) return vec2(0.0, 0.0);
    if (vid == 1u) return vec2(1.0, 0.0);
    if (vid == 2u) return vec2(1.0, 1.0);
    if (vid == 3u) return vec2(0.0, 0.0);
    if (vid == 4u) return vec2(1.0, 1.0);
    return vec2(0.0, 1.0);
}

// Quad dilation for Slug text AA: glyph quads must extend far enough beyond
// the ink bounds that every pixel the widened coverage ramp can touch gets
// rasterized — the crossing influence radius kTextFilterOuterRadiusPx
// (ui_sdf_common.glsl: exact-area reach + half the reconstruction filter).
#ifdef UI_SUBPIXEL_DUAL_SRC
// The subpixel variant samples coverage up to 1/3 px beyond the pixel centre
// (the R/B stripe taps), so edge pixels need that much extra dilation or the
// outermost fringe column would never be rasterized.
const float kSlugDilatePixels = kTextFilterOuterRadiusPx + 1.0 / 3.0;
#else
const float kSlugDilatePixels = kTextFilterOuterRadiusPx;
#endif

void main()
{
#if defined(GE_COMPAT_PROFILE)
    // Each run is its own draw, and its first draw-order index arrives in a
    // push constant. firstInstance is deliberately not the carrier: it does not
    // reach gl_InstanceIndex through SPIRV-Cross/Metal, and a silently-zero base
    // would draw every run from the head of the list (Includes/instance_io.glsl
    // records the same trap for the compat mesh path).
    uint instIndex = pc.drawBase + uint(gl_InstanceIndex);
#else
    uint instIndex = uint(gl_InstanceIndex);
#endif
    // DrawOrder indirection: dense [0..drawCount) instance index → sparse
    // persistent slot index. Vertex AND fragment must read the same primitive,
    // so we pass the resolved slot to the fragment as a flat varying (vSlot).
#if defined(GE_COMPAT_PROFILE)
    uint orderWord = sb_order.drawOrder[instIndex];
    uint slot = orderWord & UI_DRAW_ORDER_SLOT_MASK;
    vTexSlot = orderWord >> UI_DRAW_ORDER_TEX_SLOT_SHIFT;
#else
    uint slot = sb_order.drawOrder[instIndex];
#endif
    vInstance = instIndex;
    vSlot = slot;
    UIPrimitive prim = sb.prims[slot];

    vec2 p01 = quadPos01(uint(gl_VertexIndex) % 6u);

    uint mode = prim.packed.x & MODE_MASK;
    bool isSlug = (mode == MODE_SLUG) && ((prim.packed.x & COLOR_GLYPH_BIT) == 0u);

    // A glyph's effect instance paints its shadow, glow and outline inside its
    // own quad, so the quad grows by their reach (zero for a plain fill).
    float textEffectPad = mode == MODE_SLUG ? uiTextEffectReachPx(prim) : 0.0;
    if (isSlug)
    {
        // Slug text: dynamically dilate the quad by kSlugDilatePixels in
        // screen space so dilation is always correct regardless of zoom/DPI.
        // The SSBO stores undilated rect and undilated em-space bounds.

        // Expand screen-space rect outward by kSlugDilatePixels on each edge.
        vec2 rectPos = prim.rectXYWH.xy - vec2(kSlugDilatePixels + textEffectPad);
        vec2 rectSize = prim.rectXYWH.zw + vec2(2.0 * (kSlugDilatePixels + textEffectPad));

        // Compute how much em-space corresponds to kSlugDilatePixels screen pixels.
        // uvRect stores undilated em bounds: (emXMin, emYMax, emXMax, emYMin).
        // emExtent.x = emXMax - emXMin, emExtent.y = emYMax - emYMin (positive).
        vec2 emMin = vec2(prim.uvRect.x, prim.uvRect.w); // (emXMin, emYMin)
        vec2 emMax = vec2(prim.uvRect.z, prim.uvRect.y); // (emXMax, emYMax)
        vec2 emExtent = emMax - emMin;
        vec2 screenExtent = prim.rectXYWH.zw;

        // ems per pixel along each axis; guard against degenerate zero-size glyphs.
        vec2 epp = emExtent / max(screenExtent, vec2(0.001));
        vec2 deltaEm = (kSlugDilatePixels + textEffectPad) * epp;

        // Dilated em-space bounds for interpolation.
        // uvRect convention: (emXMin, emYMax, emXMax, emYMin) — Y flipped for screen Y-down.
        vec2 dilatedUVxy = vec2(emMin.x - deltaEm.x, emMax.y + deltaEm.y);
        vec2 dilatedUVzw = vec2(emMax.x + deltaEm.x, emMin.y - deltaEm.y);

        vec2 local = rectPos + p01 * rectSize;
        vLocal = local;
        vUV = mix(dilatedUVxy, dilatedUVzw, p01);

        gl_Position = clipFromPixel(local);
    }
    else if (mode == MODE_SLUG && textEffectPad > 0.0)
    {
        // Colour glyph with a shadow: widen the quad and extrapolate its atlas
        // UVs; the fragment stage bounds its samples to uvRect.
        vec2 padUV = textEffectPad * (prim.uvRect.zw - prim.uvRect.xy)
            / max(prim.rectXYWH.zw, vec2(0.001));
        vLocal = prim.rectXYWH.xy - vec2(textEffectPad)
            + p01 * (prim.rectXYWH.zw + vec2(2.0 * textEffectPad));
        vUV = mix(prim.uvRect.xy - padUV, prim.uvRect.zw + padUV, p01);
        gl_Position = clipFromPixel(vLocal);
    }
    else
    {
        // Non-Slug modes: use rect and UVs as-is from the SSBO.
        vec2 rectPos = prim.rectXYWH.xy;
        vec2 rectSize = prim.rectXYWH.zw;
        if (mode == MODE_RECT)
        {
            // The fragment's box-filter coverage (sdfCoverage) reaches half a
            // pixel beyond the outer contour, but rasterization only shades
            // pixel centres strictly inside the quad — an undilated quad
            // drops the outermost coverage column on any edge whose adjacent
            // pixel centre falls outside the rect, i.e. up to half the ink of
            // a 1px border, per edge, per sub-pixel phase. Dilate by a pixel;
            // fragments past the contour resolve to zero coverage and
            // discard. The fragment stage reads the rect from the SSBO, so
            // its geometry is untouched. (Shadow/glow reach is a separate,
            // larger pad already baked into rectXYWH by ExpandForEffects.)
            const float kRectAADilatePx = 1.0;
            rectPos -= vec2(kRectAADilatePx);
            rectSize += vec2(2.0 * kRectAADilatePx);
        }
        vec2 local = rectPos + p01 * rectSize;
        vLocal = local;
        vUV = mix(prim.uvRect.xy, prim.uvRect.zw, p01);

        gl_Position = clipFromPixel(local);
    }
}
