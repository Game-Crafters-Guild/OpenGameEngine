// Shadow sampling include for cascaded shadow maps.
//
// Provides GE_SampleShadow() which selects the appropriate cascade based on
// the fragment's view-space depth and samples the shadow map array with
// hardware PCF via a comparison sampler. Cross-cascade blending eliminates
// visible seams at split boundaries, and the cascade term fades to lit before
// MaxShadowDistance (shadow_distance_fade.glsl) instead of ending on a line.
//
// Gated by HAS_SHADOWS: when not defined, GE_SampleShadow() returns 1.0.
//
// Required bindings (set 0, must match ForwardPlus.rendergraph ShadowData resource):
//   binding 8: ShadowData (UBO, cascade VPs + splits + params)
//   binding 9: ge_shadowMapArray (sampler2DArrayShadow)

#ifndef GE_SHADOW_SAMPLING_GLSL
#define GE_SHADOW_SAMPLING_GLSL

#include "compat_profile.glsl"
#include "shadow_pcss_decisions.glsl"
#include "shadow_receiver_plane.glsl"


// Per-fragment shadow receiver mask. 1.0 = receive shadows (default), 0.0 =
// ignore them (GE_SampleShadow short-circuits to fully-lit). The forward adapter
// sets this from the per-instance receiveShadows flag before shading; callers
// that don't touch it (e.g. terrain, which gates receiveShadows its own way)
// keep the default and are unaffected.
//
// GE_RECEIVE_SHADOWS_DECLARED announces the global to code whose include of this
// header is conditional. Writers gate on the announcement, never on a restated
// copy of the include conditions: this declaration is the one authority for
// whether ge_ReceiveShadows exists in the variant being compiled.
#define GE_RECEIVE_SHADOWS_DECLARED 1
float ge_ReceiveShadows = 1.0;

// 1 when the fragment being shaded is the terrain's own surface (set by the terrain surface
// before lighting). The terrain shadow then treats it as standing on the field, whatever height
// its facet put it at, so the terrain's shadow does not follow its tessellation.
// GE_TERRAIN_SHADOW_RECEIVER_DECLARED announces it, as GE_RECEIVE_SHADOWS_DECLARED does above.
#define GE_TERRAIN_SHADOW_RECEIVER_DECLARED 1
float ge_ShadowReceiverOnTerrain = 0.0;

#ifdef HAS_SHADOWS

#ifdef GE_COMPAT_PROFILE
// Capture the geometric receiver plane before material/light control flow.
// WebGPU requires derivatives to execute uniformly. The normal offset still
// biases the center lookup; it does not change this geometric plane's slope.
vec3 ge_shadowReceiverDx = vec3(0.0);
vec3 ge_shadowReceiverDy = vec3(0.0);
#endif

layout(set = 0, binding = 8, std140) uniform ShadowData
{
    mat4 ge_shadowVP[4];   // Light view-projection per cascade
    vec4 ge_shadowSplits;  // View-space split distances (x=cascade0, y=cascade1, ...)
    vec4 ge_shadowParams;  // x=depthBias, y=normalBias, z=numCascades, w=maxShadowDistance
    vec4 ge_shadowDebug;   // x=debugMode, y=pcssActive (0=disabled, 1=PCSS on), z=frameIndex, w=pcfQuality
    // x = softness multiplier (Poisson PCF fallback only),
    // y = PCSS / Poisson disk tap count (runtime-bound loop, clamped to [8, 64]
    //     in the shader; the 8/16/32/64 set comes from the CPU setter,
    //     ShadowMapRenderFeature::SetPcssTapCount, not from this clamp),
    // z = maxPenumbraWorld (world-space cap, converted per-cascade in the shader),
    // w = receiverPlaneBias (0/1 toggle).
    vec4 ge_shadowPcss;
    // Per-cascade PCSS metrics (precomputed on CPU):
    //   x = world-per-texel (world units per shadow-map texel)
    //   y = depth span (world units, used to convert NDC depth delta to world delta)
    //   z = bindless index for this cascade's raw-depth texture (PCSS blocker search);
    //       unused under the compat profile, which reads ge_shadowMapRaw by layer
    //   w = effective light size for this cascade (= global * falloff^cascadeIdx)
    vec4 ge_shadowPcssCascades[4];
    // Per-cascade PCSS min/max pyramid (ShadowMinMaxPyramid, R = min, G = max):
    //   x = bindless index of this cascade's pyramid view; NEGATIVE means this
    //       frame has no pyramid and the PCSS filter runs unaccelerated
    //   y = mip levels in the pyramid
    //   z = base downshift — level 0 covers a (1 << z) square of shadow texels
    //   w = reserved
    vec4 ge_shadowPcssPyramid[4];
    // Frame-global filter params:
    //   x = dither basis (0 = screen space, 1 = shadow space) — ShadowDitherBasis
    //   y = authored distance fade fraction, zw = reserved (0)
    vec4 ge_shadowFilterParams;
    // The terrain's sun-space clearance map (terrain_shadow.glsl), ShadowDataGPU terrainShadow*:
    //   grid0: x, y = grid centre X, Z; z, w = terrain corner X, Z (render-origin relative)
    //   grid1: x, y = terrain extent X, Z; z, w = sun horizontal direction X, Z
    //   grid2: x = tan(elevation); y = texel (m); z, w = u of sample 0, v of line 0
    //   grid3: x = samples per line; y = lines; z = the terrain's base height (render-origin
    //          relative); w = metres per normalized height
    //   source: x = map bindless index; y = height texture bindless index; z = 1 when a map is
    //           published for this view this frame; w = map side (texels)
    // Order matters more than naming: std140 lays this block out by position, matching
    // ShadowDataGPU member for member. A mismatch here is silent garbage, not a compile error.
    vec4 ge_terrainShadowGrid0;
    vec4 ge_terrainShadowGrid1;
    vec4 ge_terrainShadowGrid2;
    vec4 ge_terrainShadowGrid3;
    uvec4 ge_terrainShadowSource;
};

layout(set = 0, binding = 9) uniform sampler2DArrayShadow ge_shadowMapArray;

#if defined(GE_COMPAT_PROFILE)
// The same cascade array as binding 9, read for depth VALUES (the PCSS blocker
// search and DPCF's occluder distances) where the full profile reads them through
// its bindless layer views. A samplerless texture: texelFetch needs no sampler, so
// it costs one sampled texture and no sampler slot per stage. WebGPU binds a
// depth32float texture to two read-only bindings at once; this one's layout entry
// is unfilterable-float (fetch-only usage), the comparison binding's is depth.
#extension GL_EXT_samplerless_texture_functions : require
layout(set = 0, binding = 51) uniform texture2DArray ge_shadowMapRaw;
#endif

// Ray-traced directional shadow mask (DirectionalShadowMode::RayTraced). Full-res R8
// written by rt_shadow_mask.comp (1 = lit, 0 = occluded), bound by name
// (RenderServices, white fallback) only on RTShadowMask-keyword passes, so
// non-RT pipelines reflect no b28 binding and this include stays bit-identical
// without the keyword.
#ifdef GE_RT_SHADOW_MASK_ENABLED
layout(set = 0, binding = 28) uniform sampler2D ge_rtShadowMask;
#endif

// MSM4 moments array (RGBA16_UNORM). Populated by msm_write +
// msm_blur_h/v passes when FilterQuality == MSM4 is selected. When MSM4
// is not the active quality, the binding is still in the descriptor
// layout but the texture content is stale / undefined — the shader
// dispatch on quality==4 only fires when CPU has confirmed valid
// moments via the BuildShadowDataGPU fallback chain.
layout(set = 0, binding = 10) uniform sampler2DArray ge_shadowMomentsArray;

// Per-cascade glass transmittance for translucent shadows (RGBA8 colour array,
// parallel to the depth cascades). The glass-only light-space tint pass
// (GE_GLASS_SHADOW_COLOR in adapter_forward.glsl) multiplicatively blends
// transmissionColor*weight here, depth-tested against the opaque cascade so only glass
// BETWEEN the light and the receiver contributes. RGB = the tint the glass lets through;
// white = no glass. Sampled with a plain linear sampler (not the depth-compare sampler).
layout(set = 0, binding = 11) uniform sampler2DArray ge_transmittanceShadowArray;

// PCSS reads raw shadow-map depth via texelFetch on the bindless texture array
// (base index in ge_shadowDebug.y). Declared once via the shared include.
#include "bindless_textures.glsl"

// Terrain occlusion of the directional light from the terrain's clearance map
// (terrain_shadow.glsl): the map and the height texture are read by bindless index from
// ShadowData, so no variant carries an extra binding. The compatibility profile has no bindless
// array and keeps the term off.
#if !defined(GE_COMPAT_PROFILE)
vec2 GE_TerrainShadowReadClearance(vec2 gridCoord)
{
    float side = float(ge_terrainShadowSource.w);
    return textureLod(GE_BTEX(ge_terrainShadowSource.x, GE_TS_CLAMP), (gridCoord + 0.5) / side, 0.0).rg;
}
float GE_TerrainShadowReadGround(float x, float z)
{
    vec2 dim = vec2(textureSize(ge_BindlessTextures[nonuniformEXT(ge_terrainShadowSource.y)], 0));
    vec2 terrainUV = clamp((vec2(x, z) - ge_terrainShadowGrid0.zw) / ge_terrainShadowGrid1.xy, 0.0, 1.0);
    return textureLod(GE_BTEX(ge_terrainShadowSource.y, GE_TS_CLAMP), (terrainUV * (dim - 1.0) + 0.5) / dim,
                      0.0).r * ge_terrainShadowGrid3.w;
}
#include "terrain_shadow.glsl"

// The share of the sun the terrain lets reach the receiver at posRel (render-origin relative): 1 lit,
// 0 in full shadow; 1 when no map is published. Under the filters that read the light's angular size
// (PCSS, DPCF) the edge softens with it as the cascades' PCSS kernel does, its radius bounded by the
// same Max Penumbra (`maxPenumbraRadius`); under the others it stays hard.
float GE_TerrainShadow(vec3 posRel, float maxPenumbraRadius)
{
    if (ge_terrainShadowSource.z == 0u)
        return 1.0;
    int quality = int(ge_shadowDebug.w + 0.5);
    float tanHalfAngle = (quality == 3 || quality == 5) ? ge_shadowPcssCascades[0].w : 0.0;
    GE_TerrainShadowGrid g;
    g.CenterX = ge_terrainShadowGrid0.x;
    g.CenterZ = ge_terrainShadowGrid0.y;
    g.TerrainX = ge_terrainShadowGrid0.z;
    g.TerrainZ = ge_terrainShadowGrid0.w;
    g.TerrainSizeX = ge_terrainShadowGrid1.x;
    g.TerrainSizeZ = ge_terrainShadowGrid1.y;
    g.SunX = ge_terrainShadowGrid1.z;
    g.SunZ = ge_terrainShadowGrid1.w;
    g.TanElevation = ge_terrainShadowGrid2.x;
    g.Texel = ge_terrainShadowGrid2.y;
    g.UMin = ge_terrainShadowGrid2.z;
    g.VMin = ge_terrainShadowGrid2.w;
    g.SamplesU = ge_terrainShadowGrid3.x;
    g.SamplesV = ge_terrainShadowGrid3.y;
    return GE_TerrainShadowLit(g, posRel.x, posRel.y - ge_terrainShadowGrid3.z, posRel.z,
                               ge_ShadowReceiverOnTerrain, tanHalfAngle, maxPenumbraRadius);
}
#else
float GE_TerrainShadow(vec3 posRel, float maxPenumbraRadius)
{
    return 1.0;
}
#endif

// Procedural caustic web (GE_CausticWeb) for the glass focused-light dapple.
#include "caustics.glsl"
#include "shadow_cascade_blend.glsl"
#include "shadow_distance_fade.glsl"
#include "shadow_filter.glsl"

// Compile-time upper bound on the runtime-variable tap count. Drivers need a
// constant loop bound for clean SPIR-V codegen; the actual iteration count is
// capped at runtime via `tapCount` (clamped to [8, 64] from ge_shadowPcss.y).
const int kPcssMaxTaps = 64;

// Per-pixel pseudo-random rotation for the Vogel disk. Returns (cosA, sinA).
//
// Interleaved gradient noise (Jimenez 2014) over a basis chosen at runtime by
// ge_shadowFilterParams.x:
//
//   Screen (0)      keys on gl_FragCoord. The noise field is nailed to the
//                   SCREEN, so geometry slides under it as the camera moves —
//                   the grain swims. High spatial frequency, which is what the
//                   other basis gives up.
//   ShadowSpace (1) keys on the shadow coordinate, welding the field to the
//                   shadow lattice (and via the centre texel snap, to the
//                   world). Under magnification adjacent screen pixels map to
//                   nearby shadow coords and so decorrelate LESS: the noise
//                   drops in frequency, reading blotchy rather than grainy.
//
// Neither is correct in general — the trade is range-dependent — so this is a
// live toggle rather than a fix, and Screen stays the default because it is
// what shipped.
//
// The shadow key is the CONTINUOUS product, never floor()ed. Flooring gives
// every screen pixel inside one shadow texel the same angle, which under
// magnification turns fine grain into texel-sized blocks — the exact artifact
// this basis is meant to avoid. (The local families in clustered_lighting.glsl
// do floor theirs; that is a known divergence, not a pattern to copy.)
//
// The temporal term feeds TAA accumulation; without TAA it would just turn the
// penumbra into per-pixel shimmer, so it stays gated on GE_TAA_ENABLED.
vec2 GE_PoissonRotation(vec2 screenPos, vec2 shadowUV, vec2 texSize)
{
    vec2 key = (ge_shadowFilterParams.x > 0.5) ? (shadowUV * texSize) : screenPos;
#ifdef GE_TAA_ENABLED
    key += ge_shadowDebug.z * 5.083;
#endif
    float angle = fract(52.9829189 * fract(dot(key, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

// Runtime tap count. This clamps the ENDS only and does not snap to the allowed
// set: ShadowMapRenderFeature::SetPcssTapCount rounds to the nearest of
// {8, 16, 32, 64} before upload, so the set is a CPU-side guarantee.
int GE_PcssTapCount()
{
    int n = int(ge_shadowPcss.y + 0.5);
    return clamp(n, 8, kPcssMaxTaps);
}

// Kernel width, in texels, from which the tap count grows past the authored one.
const float kPcssTapRefTexels = 16.0;

// Sample count scaled to how many texels the kernel actually spans.
//
// Once the penumbra is a fixed WORLD size, its width in texels varies hugely by
// cascade: a fine-texel near cascade spans many texels for the same physical
// penumbra, a coarse far cascade spans few. One global tap count therefore
// under-samples one end (banding) and wastes work at the other. The authored
// count (ge_shadowPcss.y) stays the dial — it is the count at
// kPcssTapRefTexels, so raising it shifts the whole curve rather than pinning
// a single width.
//
// Linear in radius, not quadratic: constant sample DENSITY over a disk would
// need taps proportional to area, which explodes. Linear keeps the cost bounded
// and kPcssMaxTaps bounds it absolutely.
//
// The authored count is the floor: a kernel narrower than kPcssTapRefTexels keeps
// it rather than scaling down. A physical sun's penumbra is a few texels wide (a
// 0.53 degree sun over a 2 m composition: about 4.6 texels), and scaled down that
// kernel fell to 8 taps, too few for the per-pixel rotated disk to read as
// anything but stipple. Taps lie in [authored, kPcssMaxTaps].
int GE_PcssTapsForRadius(float radiusTexels)
{
    int authored = GE_PcssTapCount();
    float scaled = float(authored) * radiusTexels / kPcssTapRefTexels;
    return clamp(int(scaled + 0.5), authored, kPcssMaxTaps);
}

// MSM4 (Peters & Klein 2015) decode matrix from supplementary listing 2,
// transposed for GLSL column-major. See msm_write.frag for the encode
// matrix; the two are NOT numerical inverses (they're an entropy-
// optimized affine pair) — both are taken verbatim from the paper.
//
// HLSL listing: raw = mul(stored, kDecode_HLSL)     (row-vec × matrix)
// GLSL:         raw = kMomentDecode * stored        (col-vec × matrix^T)
// kMomentDecode = transpose(kDecode_HLSL): each GLSL column is one HLSL row.
const mat4 kMomentDecode = mat4(
    vec4(0.2227744146, 0.1549679261, 0.1451988946,  0.163127443),
    vec4(0.0771972861, 0.1394629426, 0.2120202157,  0.2591432266),
    vec4(0.7926986636, 0.7963415838, 0.7258694464,  0.6539092497),
    vec4(0.0319417555, -0.1722823173, -0.2758014811, -0.3376131734));
const float kMsmMomentBias = 3e-5;        // pulls moments toward (0.5,0.5,0.5,0.5)
                                          // for Cholesky stability at constant-depth
                                          // regions; recommended by the paper.

// MSM4 Hamburger 4-moment shadow bound. Returns SHADOW INTENSITY (1 =
// fully shadowed, 0 = fully lit). Visibility = 1.0 - this. Math is
// listing 3 from Peters & Klein 2015 supplementary. Closed-form, no
// loops, no per-pixel rotation — runs cleanly without TAA and produces
// noise-free pre-filtered penumbras.
//
// `b` is the un-quantized 4-moment vector (z, z^2, z^3, z^4) in forward-Z
// convention. `receiverDepth` is the receiver's depth in the same forward-Z
// frame (un-reversed by the caller; see GE_SampleCascadeMsm).
float GE_HamburgerBound4(vec4 b, float receiverDepth)
{
    // MomentBias compensates quantization rounding so the Cholesky
    // factorization stays numerically positive-definite at vanishing-
    // variance regions (e.g. flat-shadowed planes where every texel has
    // an identical depth). 3e-5 is the paper's recommended value.
    b = mix(b, vec4(0.5), kMsmMomentBias);

    // Hamburger Cholesky in moment-power Hankel form.
    float L32D22     = -b[0] * b[1] + b[2];
    float D22        = -b[0] * b[0] + b[1];
    float SqDepthVar = -b[1] * b[1] + b[3];
    float D33D22     = dot(vec2(SqDepthVar, -L32D22), vec2(D22, L32D22));
    float InvD22     = 1.0 / D22;
    float L32        = L32D22 * InvD22;

    vec3 c = vec3(1.0, receiverDepth, receiverDepth * receiverDepth);
    c[1] -= b.x;
    c[2] -= b.y + L32 * c[1];
    c[1] *= InvD22;
    c[2] *= D22 / D33D22;
    c[1] -= L32 * c[2];
    c[0] -= dot(c.yz, b.xy);

    // Solve quadratic for two Dirac positions.
    float p = c[1] / c[2];
    float q = c[0] / c[2];
    float D = (p * p) * 0.25 - q;
    // Defensive against quantization noise that can drive D slightly
    // negative; the paper omits this, but production impls typically
    // include the guard. Math review punch list called this out.
    D = max(D, 0.0);
    float r = sqrt(D);
    float z1 = -p * 0.5 - r;
    float z2 = -p * 0.5 + r;

    // CDF "Switch" branch — selects which Dirac contributes, depending
    // on which side of the receiver the two reconstructed depths fall.
    vec4 sw = (z2 < receiverDepth) ? vec4(z1, receiverDepth, 1.0, 1.0) :
              (z1 < receiverDepth) ? vec4(receiverDepth, z1, 0.0, 1.0) :
                                     vec4(0.0, 0.0, 0.0, 0.0);
    float Quotient = (sw[0] * z2 - b[0] * (sw[0] + z2) + b[1])
                   / ((z2 - sw[1]) * (receiverDepth - z1));
    return clamp(sw[2] + sw[3] * Quotient, 0.0, 1.0);
}

// MSM4 sampling for a single cascade. Reads moments at (uv, cascadeIdx),
// dequantizes through the published Peters & Klein decode matrix, and
// evaluates the Hamburger 4MSM bound. Returns visibility (1 = lit,
// 0 = shadowed). `receiverDepthFZ` is in forward-Z internal convention
// (caller un-reverses).
float GE_SampleCascadeMsm(int cascadeIdx, vec2 uv, float receiverDepthFZ)
{
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
        return 1.0; // out of bounds = lit

    vec4 stored = GE_SHADOW_TAP(ge_shadowMomentsArray, vec3(uv, float(cascadeIdx)));
    // Inverse of msm_write.frag's channel-0 bias; from supplementary
    // listing 2, paired with the decode matrix below.
    stored.x -= 0.035955884801;
    vec4 raw = kMomentDecode * stored;
    float intensity = GE_HamburgerBound4(raw, receiverDepthFZ);
    return 1.0 - intensity;
}

// Blocker-search bounds, in SHADOW-MAP TEXELS — a sampling-density budget, not
// a physical size. The physical size comes from the light's angular diameter:
// searchWorld = tan(halfAngle) * searchDepthWorld. Because searchDepthWorld is a
// fraction of the cascade's depth span, and depth span and worldPerTexel both
// scale with cascade extent, the resulting texel radius is near-constant across
// cascades — measured 16.6 vs 16.0 texels per unit fraction for cascades 0 and 3
// at the 100 m / 4-cascade / 2048 default. That is the cascade invariance a
// world-space clamp was reaching for and could not deliver, because the value it
// clamped was a dimensionless tangent rather than a length.
//
// Min 2.0: below this every Vogel tap can still resolve into a single texel under
// bilinear snapping, making avgBlocker a point sample instead of an average — and
// a point sample that misses sizes the minimum kernel.
const float kPcssMinSearchTexels = 2.0;
// Max scales with sqrt(tapCount) so sample density stays roughly constant over
// the runtime 8..64 tap range; a fixed ceiling either starves 64-tap quality or
// lets 8-tap dither.
const float kPcssSearchTexelsPerRootTap = 4.0;
// Fraction of the cascade's world-space depth span the blocker search spans:
// how far behind the receiver an occluder may sit and still be found. Larger =
// softer far field, but more risk of distant occluders inflating avgBlocker into
// a halo at depth discontinuities. The resulting radius is then bounded by the
// widest PCF kernel (GE_PcssBoundedSearchTexels), so for the shipped sun the
// kernel cap, not this fraction, usually sets the searched width.
const float kPcssSearchDepthFraction = 2.0;

// Auto ceiling on the physical penumbra, in WORLD units and uniform across
// cascades. This is the whole point of the slice: the previous ceiling was
// 4*sqrt(taps) * worldPerTexel, so halving shadow resolution doubled the
// permitted penumbra in metres while the tap count stayed put. Measured on
// ShadowStress with the cascade fit held identical: median edge-width ratio
// 2.12 across 12 paired samples, 2048 vs 1024.
//
// It rarely binds for a real sun -- at 0.53 degrees a 0.5 m penumbra needs a
// 108 m caster-receiver gap -- so for the shipped light the penumbra stays
// purely physical and this is inert. It exists for wide lights, where it is the
// primary look knob; ge_shadowPcss.z (maxPenumbra, 0 = auto) overrides it per
// project. Mirrored on the CPU by kPcssMaxPenumbraWorldAuto in
// ShadowMapRenderFeature.h, which sizes the min/max pyramid for this cap.
const float kPcssMaxPenumbraWorldAuto = 0.5;

bool GE_ShadowUVInBounds(vec2 uv)
{
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

// Raw shadow-map depth for PCSS's blocker search and DPCF. `rawSource` is what
// GE_PcssRawSource returned for the cascade: under the full profile that
// cascade's bindless-texture index (the allocator does not guarantee contiguity,
// so each cascade is indexed independently); under the compat profile the
// cascade's layer in ge_shadowMapRaw.
#if defined(GE_COMPAT_PROFILE)
int GE_PcssRawSource(int cascadeIdx)
{
    return cascadeIdx;
}
float GE_PcssRawDepth(int rawSource, ivec2 coord)
{
    return texelFetch(ge_shadowMapRaw, ivec3(coord, rawSource), 0).r;
}
#else
int GE_PcssRawSource(int cascadeIdx)
{
    return int(ge_shadowPcssCascades[cascadeIdx].z);
}
float GE_PcssRawDepth(int rawSource, ivec2 coord)
{
    return texelFetch(ge_BindlessTextures[nonuniformEXT(rawSource)], coord, 0).r;
}
#endif

// PCSS blocker search: sample raw depth values to find the average occluder
// depth. Returns -1.0 if no blockers found. searchRadiusTexels scales the
// sampling disk (in shadow-map texels).
float GE_BlockerSearch(vec2 shadowUV, float refDepth, vec2 rpBias, int rawSource, vec2 screenPos,
                       float searchRadiusTexels, int tapCount)
{
    ivec2 texSize = textureSize(ge_shadowMapArray, 0).xy;
    vec2 sc = GE_PoissonRotation(screenPos, shadowUV, vec2(texSize));

    float blockerSum = 0.0;
    int blockerCount = 0;
    // refDepth already has ge_shadowParams.x (depthBias) added upstream;
    // an extra bias here would be redundant.
    // Tap count is supplied by the caller (GE_PcssTapsForRadius over this
    // search's own texel width), runtime-bound with a compile-time upper bound
    // for SPIR-V loop unrolling. More taps directly reduce avgBlocker variance,
    // which drives the per-pixel penumbra-size variance — and that variance IS
    // the PCSS dither perceived as filter noise.
    for (int i = 0; i < kPcssMaxTaps; ++i)
    {
        if (i >= tapCount) break;
        vec2 offset = GE_VogelDisk(i, tapCount, sc) * searchRadiusTexels;
        vec2 sampleUV = shadowUV + offset / vec2(texSize);
        if (!GE_ShadowUVInBounds(sampleUV))
            continue;
        ivec2 coord = ivec2(sampleUV * vec2(texSize));
        coord = clamp(coord, ivec2(0), texSize - ivec2(1));
        float d = GE_PcssRawDepth(rawSource, coord);
        if (GE_PcssIsBlocker(d, refDepth, offset / vec2(texSize), rpBias))
        {
            blockerSum += d;
            blockerCount++;
        }
    }

    return (blockerCount > 0) ? blockerSum / float(blockerCount) : -1.0;
}

// DPCF curve (Treyarch, Cold War). Pushes the occluded percentage AWAY from the
// midpoint by how close the average occluder is, which is what produces contact
// hardening without a blocker-search pass.
//
// `pcfWeight` 0 = contact hardened (occluder sitting on the receiver), 1 = plain
// PCF (occluder far away, or the receiver far from the light).
//
// The fold is the whole trick: 1 - |2p-1| is 0 at BOTH ends and 1 at the middle,
// so a power function applied there pushes toward whichever end p came from,
// and `sign` remembers which one to unfold onto.
//
// Two consequences worth naming:
//   * below 0.5 shadowing is reduced sharply — 25% occluded comes out ~6%. That
//     IS the acne reduction: a surface self-shadowing on depth noise sits near
//     0.25 and is pushed to nearly lit.
//   * the curve is IDENTITY at 0 and 1 (the fold is 0 there, and any power of 0
//     is 0). The min/max pyramid's early-outs return exactly 0.0 and 1.0, so
//     they stay exact under DPCF. Break this and the pyramid must be gated off
//     for this filter or it punches wrong pixels through solid shadow.
float GE_DpcfCurve(float percentageOccluded, float pcfWeight)
{
    float p = 2.0 * percentageOccluded - 1.0;
    float s = sign(p);
    p = 1.0 - s * p;
    p = mix(p * p * p, p, pcfWeight);
    p = 1.0 - p;
    p *= s;
    return 0.5 * p + 0.5;
}

// Query margin, in full-resolution shadow texels. The PCF taps the query covers
// do not stop at the kernel's nominal radius: each is a hardware 2x2 bilinear
// compare, reading up to 1.5 texels past its centre. Mirrored on the CPU by
// ShadowMinMaxPyramid::kQueryMarginTexels, which sizes the pyramid's levels.
const float kPcssPyramidQueryMargin = 1.5;

// Sentinel bound: the widest interval there is, so both early-out tests fail and
// the caller runs the unaccelerated filter. Returned whenever the pyramid cannot
// answer conservatively — concluding nothing is the only safe answer, since an
// under-covering bound produces a wrong pixel rather than a missed optimisation.
const vec2 kPcssPyramidNoBound = vec2(-1.0e30, 1.0e30);

// Conservative (min, max) depth over EVERY full-resolution shadow texel within
// `radiusTexels` of `shadowUV`, read from the min/max pyramid (R = min,
// G = max). `fullSize` is the shadow map's own extent.
//
// The scan takes the SMALLEST level whose footprint covers the query, so the
// bound is never read from a region smaller than what the filter samples. Where
// no level can cover it — the level cap, a non-tiling extent, a NaN radius —
// there is no conservative bound to give and the query says so.
// `baseDownshift` arrives through the UBO from ShadowMinMaxPyramid::kBaseDownshift,
// so the level indices here address the reduction shadow_minmax_reduce.comp built.
//
// The fetch is 3x3 around the level texel containing `shadowUV`, which covers a
// full level texel — `footprint` full-resolution texels — on every side. The 2x2
// a bilinear-style gather would use guarantees only half of that, and half is
// exactly the range a level whose footprint merely matches the radius provides.
vec2 GE_PcssPyramidMinMax(vec2 shadowUV, ivec2 fullSize, int pyramidIdx, int levelCount,
                          int baseDownshift, float radiusTexels)
{
#if defined(GE_COMPAT_PROFILE)
    // The pyramid is read through the bindless texture array, which this profile
    // does not have. Answering "no bound" is the same answer a non-tiling extent
    // gives: both early-outs stay unfired and the caller pays for the full
    // blocker search the pyramid exists to skip. The acceleration is lost, the
    // image is not.
    return kPcssPyramidNoBound;
#else
    float query = radiusTexels + kPcssPyramidQueryMargin;
    // NaN fails every comparison, so an unguarded scan settles on level 0 — the
    // SMALLEST footprint — and the bound stops being conservative.
    if (isnan(query))
        return kPcssPyramidNoBound;

    int level = 0;
    float footprint = float(1 << baseDownshift);
    while (footprint < query && level < levelCount - 1)
    {
        footprint *= 2.0;
        ++level;
    }
    // The level cap was hit before the footprint reached the query, so the fetch
    // below would miss texels the filter reads.
    if (footprint < query)
        return kPcssPyramidNoBound;

    int shift = baseDownshift + level;
    ivec2 levelSize = textureSize(ge_BindlessTextures[nonuniformEXT(pyramidIdx)], level);
    // A level extent that does not tile the shadow map exactly leaves trailing
    // full-resolution texels out of the reduction — the coarser level simply has
    // no texel that ever read them — so no bound taken here covers them.
    if ((levelSize.x << shift) != fullSize.x || (levelSize.y << shift) != fullSize.y)
        return kPcssPyramidNoBound;

    // Level texel t reduces full-resolution texels [t << shift, (t+1) << shift),
    // so the containing texel is the full-resolution index shifted down. Derived
    // from the index rather than from the level extent: same answer, none of the
    // rounding a separate uv-times-extent multiply would introduce.
    ivec2 centre = ivec2(shadowUV * vec2(fullSize)) >> shift;

    float lo =  1.0e30;
    float hi = -1.0e30;
    for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x)
    {
        // Clamping only re-reads an edge texel, which widens [lo, hi] and so
        // stays conservative in both directions.
        ivec2 c = clamp(centre + ivec2(x, y), ivec2(0), levelSize - ivec2(1));
        vec2 v = texelFetch(ge_BindlessTextures[nonuniformEXT(pyramidIdx)], c, level).rg;
        lo = min(lo, v.r);
        hi = max(hi, v.g);
    }
    return vec2(lo, hi);
#endif
}

// Per-cascade debug tint colors: red, green, blue, yellow.
const vec3 kCascadeDebugColors[4] = vec3[4](
    vec3(1.0, 0.2, 0.2),  // Cascade 0: red
    vec3(0.2, 1.0, 0.2),  // Cascade 1: green
    vec3(0.2, 0.2, 1.0),  // Cascade 2: blue
    vec3(1.0, 1.0, 0.2)   // Cascade 3: yellow
);

// Last cascade index and shadow result from GE_SampleShadow (for debug visualization).
int ge_lastCascadeIndex = 0;
float ge_lastShadowResult = 1.0;
float ge_lastBaseShadowResult = 1.0;
float ge_lastContactShadowResult = 1.0;

// PCSS min/max-pyramid branch classification (ShadowDebugMode::PcssBranch),
// written inside GE_SampleCascade's PCSS path and read back by
// GE_GetShadowDebugColor. NotApplicable is the default so a fragment that
// never reaches the PCSS branch this invocation (quality != 3, shadows
// disabled, receiver opted out) reports "nothing to classify" rather than a
// value some earlier call left behind.
const int kPcssBranchEarlyOutLit = 0;
const int kPcssBranchEarlyOutShadowed = 1;
const int kPcssBranchFullPath = 2;
const int kPcssBranchNotApplicable = -1;
int ge_lastPcssBranch = kPcssBranchNotApplicable;

// Glass transmittance tint at this fragment, written by GE_SampleShadow. The
// lighting multiplies the directional light by this so glass between the light
// and the surface casts a coloured, lightened shadow instead of a hard one.
// Default = no glass in the way (white = full transmission).
vec3 ge_lastShadowTint = vec3(1.0);

// Glass presence at this fragment (the tint cascade's alpha channel): 1 where glass is
// between the light and the surface, 0 elsewhere. Gates the focused-light caustic dapple
// — the tint alone can't drive it, since clear glass and no glass both read white in RGB.
float ge_lastGlassPresence = 0.0;

// One PCF tap through the hardware comparison sampler (sampler2DArrayShadow):
// a 2x2 depth compare with bilinear blending of the four binary results, so an
// N-tap kernel produces a smooth continuum of shadow factors. Raw-depth
// texelFetch compares live ONLY in GE_BlockerSearch — the PCSS blocker phase
// needs depth VALUES, not compare results. Routing factor taps through a
// point-sampled binary compare instead quantizes an N-tap kernel to N+1
// discrete levels and turns every penumbra into visible steps.
float GE_ShadowCompareTap(vec2 uv, float layer, float refDepth)
{
    if (!GE_ShadowUVInBounds(uv))
        return 1.0;
    return GE_SHADOW_TAP(ge_shadowMapArray, vec4(uv, layer, refDepth));
}

// Project a world position into a cascade's shadow clip space -> NDC (reverse-Z depth in .z).
// One source of truth for the cascade VP transform, shared by the depth and tint samplers.
vec3 GE_CascadeShadowNDC(vec3 biasedPos, int cascadeIdx)
{
    vec4 clip = ge_shadowVP[cascadeIdx] * vec4(biasedPos, 1.0);
    return clip.xyz / clip.w;
}

// NDC.xy -> shadow-map UV with the negative-viewport Y-flip the cascades rasterize with.
// Same convention as screen_position.glsl's GE_YUpNdcToViewportUV; kept local to avoid
// widening the shadow header's include surface.
vec2 GE_CascadeShadowUV(vec3 shadowNDC)
{
    vec2 uv = shadowNDC.xy * 0.5 + 0.5;
    uv.y = 1.0 - uv.y;
    return uv;
}

// Sample a single cascade with the selected filter. Grid5x5 uses a separable
// binomial kernel reconstructed with nine hardware comparison samples.
// Returns 1.0 if out-of-bounds, otherwise the filtered shadow factor.
float GE_SampleCascade(vec3 biasedPos, int cascadeIdx, vec2 screenPos,
                       vec3 receiverDx, vec3 receiverDy)
{
    vec3 shadowNDC = GE_CascadeShadowNDC(biasedPos, cascadeIdx);
    vec2 shadowUV = GE_CascadeShadowUV(shadowNDC);

    // Per-cascade scaling: keep the world-space anti-acne offset and the
    // world-space PCF kernel radius approximately CONSTANT across cascades,
    // so peter-panning and Poisson-jitter noise don't grow visibly as the
    // shadow sampler steps into a wider cascade. ge_shadowPcssCascades[i].x
    // = worldPerTexel and .y = depthSpanWorld are filled by the CPU for all
    // cascades regardless of PCSS being active. The reference is cascade 0,
    // so cascade-0 behavior is preserved exactly for backwards compat.
    float refDepthSpan = max(ge_shadowPcssCascades[0].y, 1e-3);
    float refWorldPerTexel = max(ge_shadowPcssCascades[0].x, 1e-6);
    float thisDepthSpan = max(ge_shadowPcssCascades[cascadeIdx].y, 1e-3);
    float thisWorldPerTexel = max(ge_shadowPcssCascades[cascadeIdx].x, 1e-6);
    float biasScale = refDepthSpan / thisDepthSpan;     // shrinks bias on far cascades
    float kernelScale = refWorldPerTexel / thisWorldPerTexel; // shrinks PCF on far cascades

    // Reverse-Z anti-acne bias. The shadow comparison sampler is GreaterOrEqual
    // ("lit if refDepth >= storedDepth"), and reverse-Z stores occluders with
    // LARGER NDC depth (toward 1.0 = near/light). Adding a positive bias to
    // refDepth pushes the receiver TOWARD 1.0 — i.e. toward the light — which
    // makes the surface compare "lit" against its own slightly-noisy occluder
    // depth and cancels self-shadow acne. The trade-off is peter-panning:
    // larger biases visibly detach the contact shadow from the receiver.
    // In forward-Z this same bias would subtract instead of add.
    float refDepth = shadowNDC.z + ge_shadowParams.x * biasScale;

    // Out-of-bounds: treat as fully lit.
    if (!GE_ShadowUVInBounds(shadowUV) || refDepth < 0.0 || refDepth > 1.0)
        return 1.0;

    float texelSize = 1.0 / float(textureSize(ge_shadowMapArray, 0).x);
    float layer = float(cascadeIdx);

    // Per-pixel Poisson rotation shared across all PCF paths. The blocker search
    // derives its own from the same inputs, so search and filter always agree.
    vec2 sc = GE_PoissonRotation(screenPos, shadowUV,
                                 vec2(textureSize(ge_shadowMapArray, 0).xy));

    int quality = int(ge_shadowDebug.w + 0.5);

    // MSM4: pre-filtered analytic bound. Falls back to the path below if
    // the moments aren't ready (CPU sets quality=3 in BuildShadowDataGPU
    // when MSM is requested but moments are missing). Receiver depth is
    // un-reversed to forward-Z internal convention to match the write
    // side; the existing receiver-bias (refDepth = shadowNDC.z + bias)
    // is reverse-Z, so un-reversing 1.0 - refDepth gives forward-Z with
    // the bias correctly subtracting (anti-acne in forward-Z).
    if (quality == 4)
    {
        float receiverFZ = clamp(1.0 - refDepth, 0.0001, 0.9999);
        return GE_SampleCascadeMsm(cascadeIdx, shadowUV, receiverFZ);
    }

    // DPCF: contact hardening from ONE tap set. Every tap contributes to BOTH
    // the occluded count and the occluder-distance sum, so the penumbra estimate
    // costs no extra fetch and no dependent read — the property PCSS cannot have
    // and the reason this fit a gen8 60 Hz budget. Shares PCSS's raw-depth
    // prerequisite (ge_shadowDebug.y), hence the same gate.
    if (quality == 5 && int(ge_shadowDebug.y) != 0)
    {
        float worldPerTexel = ge_shadowPcssCascades[cascadeIdx].x;
        float depthSpanWorld = ge_shadowPcssCascades[cascadeIdx].y;
        int rawSource = GE_PcssRawSource(cascadeIdx);
        float tanHalfAngle = max(ge_shadowPcssCascades[cascadeIdx].w, 0.0);

        // Kernel width is authored, not searched: DPCF never learns how far the
        // blocker is BEFORE filtering, so the radius is a fixed world size (the
        // deck's "max percentage of shadowmap") rather than a derived penumbra.
        float userCapWorld = max(ge_shadowPcss.z, 0.0);
        float radiusWorld = (userCapWorld > 0.0) ? userCapWorld : kPcssMaxPenumbraWorldAuto;
        float radiusTexels = max(radiusWorld / max(worldPerTexel, 1e-6), 1.0);
        int tapCount = GE_PcssTapsForRadius(radiusTexels);

        ivec2 texSize = textureSize(ge_shadowMapArray, 0).xy;
        float occluders = 0.0;
        float occluderDistSum = 0.0;
        float sampled = 0.0;
        for (int i = 0; i < kPcssMaxTaps; ++i)
        {
            if (i >= tapCount) break;
            vec2 offset = GE_VogelDisk(i, tapCount, sc) * radiusTexels;
            vec2 sampleUV = shadowUV + offset / vec2(texSize);
            if (!GE_ShadowUVInBounds(sampleUV))
                continue; // out of the map reads as lit, matching the PCSS path
            ivec2 coord = clamp(ivec2(sampleUV * vec2(texSize)), ivec2(0), texSize - ivec2(1));
            float d = GE_PcssRawDepth(rawSource, coord);
            // Reverse-Z: a stored depth LARGER than the receiver is nearer the
            // light, so dist > 0 marks a blocker — the same convention the
            // source relies on ("as our SM depth is inverted").
            float dist = d - refDepth;
            float occluder = step(0.0, dist);
            occluders += occluder;
            occluderDistSum += dist * occluder;
            sampled += 1.0;
        }

        if (sampled < 0.5 || occluders < 0.5)
            return 1.0; // nothing sampled, or nothing occluding: fully lit

        float percentageOccluded = clamp(occluders / sampled, 0.0, 1.0);
        float occluderAvgDist = occluderDistSum / occluders;

        // The source divides the average occluder distance by
        // lightDistanceNormalized, because for a PUNCTUAL light the same world
        // gap subtends a smaller angle the further away it is. A directional
        // light is at infinity and has no such falloff — and in a cascade
        // refDepth is a position inside an ortho depth slab, not a distance from
        // the light, so that ratio would carry no physical meaning here.
        //
        // The directional analogue of "is the occluder close" is how much of the
        // KERNEL the would-be penumbra fills: 0 when the occluder sits on the
        // receiver (contact hardened), 1 once the physical penumbra is as wide as
        // the filter, where DPCF should relax to plain PCF anyway.
        float depthDeltaWorld = occluderAvgDist * depthSpanWorld;
        float penumbraWorld = depthDeltaWorld * tanHalfAngle;
        float pcfWeight = clamp(penumbraWorld / max(radiusWorld, 1e-6), 0.0, 1.0);

        return 1.0 - GE_DpcfCurve(percentageOccluded, pcfWeight);
    }

    if (quality == 3 && int(ge_shadowDebug.y) != 0)
    {
        // PCSS: three-phase contact-hardening shadows.
        //   1) Blocker search reads raw depth (GE_PcssRawDepth).
        //   2) Penumbra size derived from (refDepth - avgBlocker) scaled by
        //      the physical light size, converted to shadow-map texels using
        //      the cascade's world-per-texel and world-space depth span.
        //   3) Variable-radius Poisson PCF via the hardware compare sampler.
        float worldPerTexel = ge_shadowPcssCascades[cascadeIdx].x;
        float depthSpanWorld = ge_shadowPcssCascades[cascadeIdx].y;
        int rawSource = GE_PcssRawSource(cascadeIdx);
        // tan(half angular diameter) of the light — dimensionless, NOT a length.
        float tanHalfAngle = max(ge_shadowPcssCascades[cascadeIdx].w, 0.0);

        // Project geometry derivatives captured before per-pixel branches.
        // Derivatives of a normal-map-dependent biased position would describe
        // the bias field rather than the receiver plane we want to compare.
        vec2 rpBias = vec2(0.0);
        if (ge_shadowPcss.w > 0.5)
        {
            vec3 dx = mat3(ge_shadowVP[cascadeIdx]) * receiverDx;
            vec3 dy = mat3(ge_shadowVP[cascadeIdx]) * receiverDy;
            vec3 uvDepthScale = vec3(0.5, -0.5, 1.0) / ge_shadowVP[cascadeIdx][3][3];
            rpBias = GE_ShadowReceiverPlaneGradient(dx * uvDepthScale, dy * uvDepthScale);
        }

        // Blocker search radius: the light's angular extent projected over the
        // depth range we are willing to search. No divide by blocker distance —
        // a directional light is at infinity. The bound stays in TEXELS because
        // what it governs is tap density — bounding it in metres would make the
        // authored angular diameter unreachable, which is a defect this file has
        // already shipped once.
        //
        // What changed is where the bound comes from. It used to be
        // 4*sqrt(AUTHORED taps), a constant that cannot know the cascade's texel
        // size, so when it bound it did so at a fixed texel count and therefore
        // a resolution-dependent WORLD radius — moving avgBlocker, and the
        // penumbra estimate with it. Deriving it from the ADAPTIVE tap count
        // makes the budget track the width actually requested, so the clamp
        // rarely binds and the searched world area comes out the same at 1024
        // and 2048.
        float searchDepthWorld = depthSpanWorld * kPcssSearchDepthFraction;
        float searchWorld = tanHalfAngle * searchDepthWorld;
        int searchTaps = GE_PcssTapsForRadius(searchWorld / max(worldPerTexel, 1e-6));
        float maxSearchTexels = kPcssSearchTexelsPerRootTap * sqrt(float(searchTaps));
        float searchTexels = clamp(searchWorld / max(worldPerTexel, 1e-6),
                                   kPcssMinSearchTexels, maxSearchTexels);

        // Kernel bound, in WORLD units. It has to be: a bound expressed in
        // texels makes the penumbra's physical size a function of shadow-map
        // resolution, which is exactly the defect this slice removes. Tap
        // density is a real constraint too, but it is answered by scaling the
        // TAP COUNT to the resulting texel width (GE_PcssTapsForPenumbra), not
        // by shrinking the penumbra until the fixed tap budget copes.
        //
        // An explicit cap means exactly that -- min()-ing it with an internal
        // budget would let the budget silently veto an authored value, which is
        // the class of bug the old 0.04 m cap was: a knob that did nothing.
        float userCapWorld = max(ge_shadowPcss.z, 0.0);
        float effectiveCapWorld = (userCapWorld > 0.0) ? userCapWorld : kPcssMaxPenumbraWorldAuto;
        // The widest kernel the PCF loop below can pick, in texels and in UV.
        float pcfCapTexels = max(effectiveCapWorld, worldPerTexel) / max(worldPerTexel, 1e-6);
        float pcfCapUV = pcfCapTexels * texelSize;

        // The search only sizes the kernel, and the kernel is clamped to the cap,
        // so it is bounded by the cap too. A blocker beyond the cap still entered
        // the average blocker depth of the wider disk and so moved the kernel of
        // fragments with nearer blockers too; bounded, the kernel depends only on
        // blockers within the cap, and every fragment stops paying for the taps
        // of the wider disk. Its taps follow the bounded width.
        searchTexels = GE_PcssBoundedSearchTexels(searchTexels, pcfCapTexels, kPcssMinSearchTexels);
        searchTaps = GE_PcssTapsForRadius(searchTexels);

        // Early-outs from the min/max pyramid (ShadowMinMaxPyramid). A NEGATIVE
        // index is the "no pyramid" sentinel, and zero levels means the same
        // thing: PCSS is live on frames no pyramid was declared for (an
        // MSM4-without-moments promotion, a bindless registration that failed
        // after the build), so its presence is never inferred from PCSS being
        // active. Slot 0 is a legal-looking bindless index, which is why the
        // sentinel is negative and why this test cannot be against zero.
        //
        // The pyramid is the only way out as lit: it PROVES that nothing within
        // the widest kernel occludes any tap, so the PCF loop would return 1 for
        // every kernel it could pick. Without that proof the filter always runs,
        // because the sparse blocker search finding nothing is not a proof.
        // Receiver derivatives were already captured before entering any
        // per-pixel shadow branch, so classification cannot alter a neighbouring
        // fragment's receiver-plane correction.
        bool fullyShadowed = false;
        int pyramidIdx = int(ge_shadowPcssPyramid[cascadeIdx].x);
        int pyramidLevels = int(ge_shadowPcssPyramid[cascadeIdx].y);
        int pyramidBaseShift = int(ge_shadowPcssPyramid[cascadeIdx].z);
        if (pyramidIdx >= 0 && pyramidLevels > 0)
        {
            ivec2 shadowSize = textureSize(ge_shadowMapArray, 0).xy;
            vec2 mmKernel = GE_PcssPyramidMinMax(shadowUV, shadowSize, pyramidIdx, pyramidLevels,
                                                 pyramidBaseShift, pcfCapTexels);
            if (GE_PcssKernelProvesLit(mmKernel.y, refDepth, pcfCapUV, rpBias))
            {
                ge_lastPcssBranch = kPcssBranchEarlyOutLit;
                return 1.0;
            }

            // Fully shadowed: everything within the widest kernel is nearer the
            // light than any tap's own reference, so every compare returns 0 and
            // their average is exactly 0. The L1 gradient bound covers every tap
            // within the maximum radius, including rotated taps near the map
            // boundary, and a tap outside the shadow map reads as fully LIT, so
            // the verdict only holds while the whole kernel is in bounds.
            float refConservative = refDepth + pcfCapUV * (abs(rpBias.x) + abs(rpBias.y));
            if (GE_ShadowUVInBounds(shadowUV - vec2(pcfCapUV)) &&
                GE_ShadowUVInBounds(shadowUV + vec2(pcfCapUV)))
                fullyShadowed = mmKernel.x > refConservative;
        }
        if (fullyShadowed)
        {
            ge_lastPcssBranch = kPcssBranchEarlyOutShadowed;
            return 0.0;
        }

        // Committed to the filter: the search sizes the kernel (the minimum one
        // when it finds no blocker) and the PCF loop below decides the result.
        ge_lastPcssBranch = kPcssBranchFullPath;
        float avgBlocker = GE_BlockerSearch(shadowUV, refDepth, rpBias, rawSource, screenPos,
                                            searchTexels, searchTaps);
        float penumbraTexels = GE_PcssPenumbraTexels(avgBlocker, refDepth, depthSpanWorld, tanHalfAngle,
                                                     worldPerTexel, effectiveCapWorld);

        float filterRadius = penumbraTexels * texelSize;
        float shadow = 0.0;
        // Taps track the kernel's width in texels, which after the world-unit
        // cap varies by cascade for one physical penumbra. A fixed count would
        // band the fine-texel near cascade and overpay on the coarse far one.
        int filterTaps = GE_PcssTapsForRadius(penumbraTexels);
        for (int i = 0; i < kPcssMaxTaps; ++i)
        {
            if (i >= filterTaps) break;
            vec2 rotated = GE_VogelDisk(i, filterTaps, sc) * filterRadius;
            float tapDepth = refDepth + dot(rotated, rpBias);
            shadow += GE_ShadowCompareTap(shadowUV + rotated, layer, tapDepth);
        }
        return shadow / float(filterTaps);
    }

    if (quality == 2)
    {
        // Variable-tap Vogel disk with per-pixel rotation.
        // Tap count shared with PCSS via ge_shadowPcss.y so a single setting
        // controls all soft-shadow paths. Smaller radius = less dithering,
        // tighter shadow edge. kernelScale shrinks the radius on far cascades
        // so the world-space disk stays roughly constant.
        float radius = 1.5 * texelSize * max(ge_shadowPcss.x, 0.0) * kernelScale;

        // Where the screen-pixel floor (GE_ShadowFilterFloorTexels) widens the disk,
        // its taps follow the receiver's plane in depth, as the grids' do, so a
        // sloped receiver does not shadow itself across the wider disk. Elsewhere
        // the kernel is unchanged.
        vec2 texSize = vec2(textureSize(ge_shadowMapArray, 0).xy);
        mat3 toShadow = mat3(ge_shadowVP[cascadeIdx]);
        vec3 floorDx = (toShadow * receiverDx) * vec3(0.5, -0.5, 1.0);
        vec3 floorDy = (toShadow * receiverDy) * vec3(0.5, -0.5, 1.0);
        float floorRadius = GE_ShadowFilterFloorTexels(floorDx.xy, floorDy.xy, texSize) * texelSize;
        bool floorBinds = floorRadius > radius;
        radius = max(radius, floorRadius);
        vec2 floorGradient = floorBinds ? GE_ShadowReceiverPlaneGradient(floorDx, floorDy) : vec2(0.0);

        float shadow = 0.0;
        int poissonTaps = GE_PcssTapCount();
        for (int i = 0; i < kPcssMaxTaps; ++i)
        {
            if (i >= poissonTaps) break;
            vec2 rotated = GE_VogelDisk(i, poissonTaps, sc) * radius;
            float tapDepth = GE_FlooredKernelTapDepth(floorBinds, refDepth, rotated, floorGradient, texelSize);
            shadow += GE_ShadowCompareTap(shadowUV + rotated, layer, tapDepth);
        }
        return shadow / float(poissonTaps);
    }

    // Transform the same receiver tangents separately for each cascade.
    mat3 receiverToShadow = mat3(ge_shadowVP[cascadeIdx]);
    vec3 shadowDx = (receiverToShadow * receiverDx) * vec3(0.5, -0.5, 1.0);
    vec3 shadowDy = (receiverToShadow * receiverDy) * vec3(0.5, -0.5, 1.0);
    vec2 receiverGradient = GE_ShadowReceiverPlaneGradient(shadowDx, shadowDy);
    if (quality != 1)
        return GE_ShadowPcf5x5(ge_shadowMapArray, shadowUV, layer, refDepth, receiverGradient);

    // Grid3x3 keeps its compact box kernel.
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            float sampleDepth = GE_ShadowReceiverPlaneDepth(refDepth, offset, receiverGradient, texelSize);
            shadow += GE_ShadowCompareTap(shadowUV + offset, layer, sampleDepth);
        }
    }

    return shadow / 9.0;
}

// Sample the per-cascade glass transmittance at the same light-space UV as the depth
// shadow. Returns the RGB tint the glass lets through plus the glass-presence mask in .a
// (1 where glass is between the light and the surface). Shares the cascade projection
// (GE_CascadeShadowNDC/UV) with GE_SampleCascade so the tint registers with the depth
// occlusion. Out of bounds = white tint, no presence (leaves the light intact).
vec4 GE_SampleCascadeTint(vec3 biasedPos, int cascadeIdx)
{
    vec2 shadowUV = GE_CascadeShadowUV(GE_CascadeShadowNDC(biasedPos, cascadeIdx));
    if (!GE_ShadowUVInBounds(shadowUV))
        return vec4(1.0, 1.0, 1.0, 0.0);
    return GE_SHADOW_TAP(ge_transmittanceShadowArray, vec3(shadowUV, float(cascadeIdx)));
}

// Sample the cascaded shadow map for a receiver position expressed in the
// RENDER-ORIGIN-RELATIVE frame (Earth-scale precision): ge_shadowVP is the
// camera-relative cascade VP, so `posWS` here must be the render-origin-relative
// position (vPosRel), NOT the full world position. They are equal when the
// origin is inactive, so near-origin scenes are byte-identical. The name is kept
// for call-site stability; only the frame changed.
// lightDirWS: normalized direction *toward* the light (i.e. -lightDirection).
// screenPos: gl_FragCoord.xy for per-pixel PCF jitter.
// Returns 1.0 (fully lit) to 0.0 (fully shadowed), with cross-cascade blending
// near split boundaries and the distance fade before MaxShadowDistance.
#ifdef GE_SCREEN_SPACE_SHADOWS_ENABLED
layout(set = 0, binding = 48) uniform usampler2D ge_screenSpaceShadowMask;
// Set before fragment control flow diverges; WGSL rejects derivatives inside
// the per-light/receiver branches below.
float ge_contactReceiverDepthFootprint = 0.0;
#endif
float GE_ScreenSpaceShadow(float shadow, float linearDepth, vec2 screenPos)
{
    ge_lastBaseShadowResult = shadow;
    ge_lastContactShadowResult = 1.0;
#if defined(GE_SCREEN_SPACE_SHADOWS_ENABLED) && !defined(GE_TRANSMISSION_ENABLED)
    ivec2 pixel = ivec2(screenPos);
    if (ge_shadowParams.w > 0.0 && linearDepth < ge_shadowParams.w &&
        all(greaterThanEqual(pixel, ivec2(0))) &&
        all(lessThan(pixel, textureSize(ge_screenSpaceShadowMask, 0))))
    {
        uint packed = texelFetch(ge_screenSpaceShadowMask, pixel, 0).r;
        float receiverDepth = uintBitsToFloat(packed & 0xffffff00u);
        float visibility = float(packed & 255u) / 255.0;
        // Only the depth-prepass receiver owns this value. This also rejects
        // phase-B recovered surfaces and transparent layers at another depth.
        // The resolved MSAA depth comes from a sample within this pixel,
        // whereas the lighting invocation can evaluate depth at its center.
        // Allow half a pixel of the receiver's own depth gradient as well as
        // the packed-depth error; precision alone rejects sloping receivers.
        float tolerance = max(abs(gl_FragCoord.z) * 3.1e-5, 1e-12)
                        + 0.5 * ge_contactReceiverDepthFootprint;
        if (receiverDepth > 0.0 && abs(receiverDepth - gl_FragCoord.z) <= tolerance)
        {
            float fade = GE_ShadowDistanceFade(linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y);
            ge_lastContactShadowResult = mix(visibility, 1.0, fade);
        }
    }
#endif
    int mode = int(ge_shadowDebug.x + 0.5);
    if (mode == 6)
    {
        ge_lastShadowTint = vec3(1.0);
        ge_lastGlassPresence = 0.0;
        return ge_lastContactShadowResult;
    }
    if (mode == 7)
        return shadow;
    return min(shadow, ge_lastContactShadowResult);
}

float GE_SampleShadow(vec3 posWS, vec3 normalWS, float linearDepth, vec3 lightDirWS, vec2 screenPos)
{
    // Reset the tint + presence up front so EVERY early-return path leaves the light
    // untinted (white) with no caustic, and nothing leaks if this is ever called twice.
    ge_lastShadowTint = vec3(1.0);
    ge_lastGlassPresence = 0.0;
    ge_lastShadowResult = 1.0;
    ge_lastBaseShadowResult = 1.0;
    ge_lastContactShadowResult = 1.0;

#ifndef GE_RT_SHADOW_MASK_ENABLED
    // Evaluate derivatives before receive-mask, distance, cascade selection,
    // blocker-search and pyramid branches diverge within a fragment quad.
    vec3 receiverDx = vec3(0.0);
    vec3 receiverDy = vec3(0.0);
    #ifdef GE_COMPAT_PROFILE
    receiverDx = ge_shadowReceiverDx;
    receiverDy = ge_shadowReceiverDy;
    #else
    if (int(ge_shadowDebug.w + 0.5) <= 1 ||
        (ge_shadowPcss.w > 0.5 && int(ge_shadowDebug.w + 0.5) == 3))
    {
        receiverDx = GE_DFDX(posWS);
        receiverDy = GE_DFDY(posWS);
    }
    #endif
#endif

    // Per-instance receiveShadows gate (set by the forward adapter). Off = lit.
    if (ge_ReceiveShadows < 0.5)
        return 1.0;

#ifdef GE_RT_SHADOW_MASK_ENABLED
    // Ray-traced directional factor: the screen-space mask replaces cascade
    // selection/PCF entirely (the ONE consumption branch of the RT lane).
    // texelFetch at the fragment's own pixel — the mask is full-res and
    // screen-registered with the prepass depth it was traced from. Glass
    // transmittance tint intentionally stays at its white reset (the mask
    // carries binary occlusion only — enumerated prototype residual), and
    // hard edges replace the PCF penumbra.
    float rtShadow = texelFetch(ge_rtShadowMask, ivec2(screenPos), 0).r;
    rtShadow = GE_ScreenSpaceShadow(rtShadow, linearDepth, screenPos);
    ge_lastShadowResult = rtShadow;
    return rtShadow;
#else

    int numCascades = int(ge_shadowParams.z);
    if (numCascades <= 0)
        return 1.0;

    // The PCSS kernel's radius bound, which the terrain's penumbra shares.
    float maxPenumbraWorld = ge_shadowPcss.z > 0.0 ? ge_shadowPcss.z : kPcssMaxPenumbraWorldAuto;

    // Beyond max shadow distance the cascades hold nothing (no cascade work; inside
    // it their term fades out over the last band, GE_ShadowDistanceFade); the
    // terrain's shadow, which has no range, still reaches.
    if (linearDepth > ge_shadowParams.w)
    {
        float farTerrain = GE_TerrainShadow(posWS, maxPenumbraWorld);
        ge_lastBaseShadowResult = farTerrain;
        ge_lastShadowResult = farTerrain;
        return farTerrain;
    }

    // Select cascade by comparing linear depth against split distances.
    int cascadeIdx = numCascades - 1;
    for (int i = 0; i < numCascades; ++i)
    {
        if (linearDepth < ge_shadowSplits[i])
        {
            cascadeIdx = i;
            break;
        }
    }

    // Track cascade index for debug visualization.
    ge_lastCascadeIndex = cascadeIdx;

    // Cross-cascade blending: when the fragment is near the end of the current
    // cascade's range, blend both the footprint used for bias and the shadow
    // factors. Both maps must sample the SAME position in the transition.
    bool inBlendZone = false;
    float cascadeWeight = 1.0;
    float texelWorld = ge_shadowPcssCascades[cascadeIdx].x;
    if (cascadeIdx < numCascades - 1)
    {
        float splitDist = ge_shadowSplits[cascadeIdx];
        float cascadeStart = (cascadeIdx == 0) ? 0.0 : ge_shadowSplits[cascadeIdx - 1];
        float cascadeRange = splitDist - cascadeStart;
        float blendBand = GE_CascadeBlendBand(cascadeRange);
        float distToSplit = splitDist - linearDepth;

        if (distToSplit < blendBand)
        {
            inBlendZone = true;
            cascadeWeight = smoothstep(0.0, 1.0, distToSplit / blendBand);
            texelWorld = mix(ge_shadowPcssCascades[cascadeIdx + 1].x, texelWorld, cascadeWeight);
        }
    }

    float normalBias = GE_ShadowNormalBiasWorld(ge_shadowParams.y, texelWorld,
        ge_shadowPcssCascades[0].x, int(ge_shadowDebug.w + 0.5), ge_shadowPcss.x,
        dot(normalWS, lightDirWS),
        maxPenumbraWorld,
        ge_shadowPcss.w > 0.5,
        GE_ShadowFilterFloorWorld(receiverDx, receiverDy, texelWorld));
    vec3 biasedPos = posWS + normalWS * normalBias;
    float shadow = GE_SampleCascade(biasedPos, cascadeIdx, screenPos, receiverDx, receiverDy);
    if (inBlendZone)
    {
        float nextShadow = GE_SampleCascade(biasedPos, cascadeIdx + 1, screenPos, receiverDx, receiverDy);
        shadow = mix(nextShadow, shadow, cascadeWeight);
    }
    float distanceFade = GE_ShadowDistanceFade(linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y);

    // Glass transmittance is independent of the binary occlusion: a lit floor
    // UNDER glass is still tinted (the glass attenuates the light reaching it),
    // so sample the tint regardless of the shadow factor. Primary cascade only —
    // the tint is low-frequency, so the blend-zone seam doesn't show. The .a channel
    // carries the glass-presence mask that gates the caustic dapple.
    vec4 tintSample = GE_SampleCascadeTint(biasedPos, cascadeIdx);
    ge_lastShadowTint = mix(tintSample.rgb, vec3(1.0), distanceFade);
    ge_lastGlassPresence = tintSample.a * (1.0 - distanceFade);

    // The terrain is not in the cascades; its term joins the meshes' by a minimum, after the
    // distance fade, which ends the cascade term only (GE_JoinTerrainAfterFade). Skipped where the
    // cascades shadow the receiver fully and the fade has not begun.
    if (shadow > 0.0 || distanceFade > 0.0)
        shadow = GE_JoinTerrainAfterFade(shadow, GE_TerrainShadow(posWS, maxPenumbraWorld), linearDepth, ge_shadowParams.w, ge_shadowFilterParams.y);

    shadow = GE_ScreenSpaceShadow(shadow, linearDepth, screenPos);
    ge_lastShadowResult = shadow;
    return shadow;
#endif // GE_RT_SHADOW_MASK_ENABLED
}

// Returns true if shadow debug visualization is active.
// When mode=1 (cascade colors), outColor is the cascade tint.
// When mode=2 (shadow factor), outColor is the grayscale shadow factor.
// When mode=3 (PCSS branch), outColor is a flat green/red/blue classification
// of which PCSS early-out (if any) this fragment took.
// The caller should replace finalColor with outColor when this returns true.
bool GE_GetShadowDebugColor(float shadowFactor, out vec3 outColor)
{
    outColor = vec3(0.0);
    int mode = int(ge_shadowDebug.x + 0.5);
    if (mode == 1)
    {
        // Cascade color tint: blend the cascade color with the shadow factor.
        int idx = clamp(ge_lastCascadeIndex, 0, 3);
        outColor = kCascadeDebugColors[idx] * mix(0.3, 1.0, shadowFactor);
        return true;
    }
    else if (mode == 2)
    {
        // Shadow factor overlay: grayscale, ignoring lighting.
        outColor = vec3(shadowFactor);
        return true;
    }
    else if (mode == 4 || mode == 5)
    {
        outColor = vec3(mode == 4 ? ge_lastBaseShadowResult : ge_lastContactShadowResult);
        return true;
    }
    else if (mode == 3)
    {
        // PCSS branch classification: flat, UNMODULATED colors (not scaled by
        // shadowFactor) so the fully-shadowed early-out — whose factor is
        // exactly 0.0 — reads as solid red instead of vanishing into black.
        // A fragment the PCSS branch never classified this invocation
        // (quality != 3, shadows off, receiver opted out) falls through to
        // normal shading instead of claiming a color for nothing.
        if (ge_lastPcssBranch == kPcssBranchEarlyOutLit)
        {
            outColor = vec3(0.0, 1.0, 0.0); // green: early-out fully lit
            return true;
        }
        else if (ge_lastPcssBranch == kPcssBranchEarlyOutShadowed)
        {
            outColor = vec3(1.0, 0.0, 0.0); // red: early-out fully shadowed
            return true;
        }
        else if (ge_lastPcssBranch == kPcssBranchFullPath)
        {
            outColor = vec3(0.0, 0.0, 1.0); // blue: full penumbra path
            return true;
        }
        return false;
    }
    return false;
}

#else

// No-op fallback when shadows are disabled.
float GE_SampleShadow(vec3 posWS, vec3 normalWS, float linearDepth, vec3 lightDirWS, vec2 screenPos) { return 1.0; }
bool GE_GetShadowDebugColor(float shadowFactor, out vec3 outColor) { outColor = vec3(0.0); return false; }

#endif // HAS_SHADOWS

#endif // GE_SHADOW_SAMPLING_GLSL
