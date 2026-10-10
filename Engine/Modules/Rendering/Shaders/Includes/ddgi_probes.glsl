// DDGI forward-pass consumer (M2-M6): 8-probe trilinear irradiance lookup
// from the atlas DDGIProbeFeature maintains, weighted by a Chebyshev
// (variance shadow map style) visibility test against the depth-moment atlas
// and by each corner probe's classify active flag (Includes/ddgi_common.glsl's
// doc on ddgi_classify.comp) — both zero out or attenuate a corner that is
// occluded from the shading point or embedded in geometry, instead of
// letting it silently leak light through a wall or contribute a buried,
// meaningless sample. Declared only under GE_DDGI_ENABLED (gtao_consume.glsl's
// template) so non-DDGI variants never grow the descriptor layout.
// `ge_ddgiEnabled` covers the runtime case where the KEYWORD is compiled in
// (some view in the pipeline uses DDGI) but THIS world has no active
// DDGIVolume — the fallback binder still supplies a valid (but zero-weighted)
// UBO so the sample is always well-defined.
//
// M5 adds an optional second, finer cascade (C1, DDGIVolumeFine/ge_ddgi*Fine)
// covering a smaller sub-volume centered on the coarse grid (C0). The two
// cascades' sampling loops (GE_DDGISampleC0/GE_DDGISampleC1 below) are
// near-identical but NOT shared behind one function: GLSL's storage-buffer
// blocks are not opaque types the way sampler2D is, so a per-cascade SSBO
// binding cannot be passed as a function parameter — only the literal
// per-corner math is factored out (GE_DDGIChebyshevVisibility, which lives in
// ddgi_common.glsl because the trace kernels' recursive bounce fetch weights
// its own 8-probe gather with the same rule). Blending is
// spatial, not energy-based: full C1 near its center, fading linearly to C0
// over GE_DDGI_FINE_CASCADE_FEATHER of its half-extent, pure C0 outside —
// avoids the physically murky question of how to merge two different
// probe-count/hysteresis fields by weight.
//
// Reflections add TWO Phong lobes baked from the SAME traced rays diffuse
// uses — power 8 at 6x6 (ddgi_rough_blend.comp) and power 64 at 16x16
// (ddgi_glossy_blend.comp) — sampled here by GE_SampleDDGIReflectionBoth at
// the reflection direction rather than the surface normal, and crossfaded by
// roughness in the consumer. Both cascades bake the lobes from their own traced rays when
// EnableGlossy is on; the composite against the prefiltered environment cube
// lives in Includes/ibl.glsl, not here. C0/C1 blend uses the same spatial
// feather as irradiance (GE_SampleDDGIIrradiance).
//
// Remaining simplification (closed by later milestones): no distance-based
// edge fade at C0's OWN grid boundary (a look-dev pass). Positions outside
// C0's grid clamp to the nearest boundary probes rather than fading to
// black — a soft degrade, not a hard cutoff.

#ifndef GE_DDGI_PROBES_GLSL
#define GE_DDGI_PROBES_GLSL

#include "ddgi_common.glsl"

// On the compat FRAGMENT (ibl.glsl defines GE_DDGI_RESOLVE_ONLY before this
// include) the probe atlases, probe-state buffers and the whole inline gather
// are NOT declared: that stage reads only the screen-space resolve textures
// (ibl.glsl b41-43) plus this volume UBO, which is what keeps it inside
// WebGPU's 16-sampler-per-stage budget and off the b29-b40 range the compat
// forward+ set already spends elsewhere. Every OTHER includer — the resolve/
// blend/trace COMPUTES, and the full-profile fragment — includes this file
// WITHOUT that define and gets the full layout below, unchanged.
//
// The volume UBO is the one binding both paths share. It moves off b30 on the
// resolve-only path because ge_iblSampler sits at b30 in the compat fragment;
// the compute stages have b30 free, so they keep it there. The binder resolves
// it by its reflected name "DDGIVolume", so the number move needs no CPU change.
#if defined(GE_DDGI_RESOLVE_ONLY)
layout(std140, set = 0, binding = 44) uniform DDGIVolumeData
{
    vec4 uGridMinWS;
    vec4 uGridSizeWS;
    ivec4 uProbeCount;   // xyz = res; w = probeTotal
    vec4 uParams0;       // x=minCellWS, y=normalBiasScale, z=chebyshevStrength, w=classifyStrength
    vec4 uParams1;       // x=enabled (0/1), y=intensity, z=glossyEnabled (0/1), w=reflectionIntensity
    ivec4 uParams2;      // x=glossyTilesX, yz=glossy atlas size (texels),
                         // w=1 while the scaled glossy resolve is active
                         // (ibl.glsl taps ge_ddgiResolveRough/Glossy instead
                         // of the inline gather; only this C0 block's w is read)
} DDGIVolume;
#else

layout(set = 0, binding = 29) uniform sampler2D ge_ddgiIrradianceAtlas;

layout(std140, set = 0, binding = 30) uniform DDGIVolumeData
{
    vec4 uGridMinWS;
    vec4 uGridSizeWS;
    ivec4 uProbeCount;   // xyz = res; w = probeTotal
    vec4 uParams0;       // x=minCellWS, y=normalBiasScale, z=chebyshevStrength, w=classifyStrength
    vec4 uParams1;       // x=enabled (0/1), y=intensity, z=glossyEnabled (0/1), w=reflectionIntensity
    ivec4 uParams2;      // x=glossyTilesX, yz=glossy atlas size (texels),
                         // w=1 while the scaled glossy resolve is active
                         // (ibl.glsl taps ge_ddgiResolveRough/Glossy instead
                         // of the inline gather; only this C0 block's w is read)
    ivec4 uParams3;      // x=Components::DDGIDebugView (0 = off); read only by
                         // the resolve kernel, never by a forward fragment
                         // y=depth atlas interior tile resolution (GE_DDGIDepthTexelUV)
} DDGIVolume;

layout(set = 0, binding = 31) uniform sampler2D ge_ddgiDepthAtlas;

// xyz = per-probe relocation offset (world-space), w = active (1.0) /
// inactive (0.0) — Includes/ddgi_common.glsl's doc on ddgi_classify.comp,
// which owns this buffer's only write.
layout(std430, set = 0, binding = 32) readonly buffer DDGIProbeStateRO
{
    vec4 ge_ddgiProbeState[];
};

// M5: fine cascade (C1) — same shape, independent bindings/state. Only
// meaningful when DDGIVolumeFine.uParams1.x > 0.5 (converged AND
// EnableFineCascade — see DDGIProbeFeature::UploadVolumeDataFine); the
// world-pass binder supplies a valid but zero-weighted UBO otherwise, same
// fallback contract as the coarse grid's own DDGIVolumeData.
layout(set = 0, binding = 33) uniform sampler2D ge_ddgiIrradianceAtlasFine;
layout(std140, set = 0, binding = 34) uniform DDGIVolumeDataFine
{
    vec4 uGridMinWS;
    vec4 uGridSizeWS;
    ivec4 uProbeCount;
    vec4 uParams0;
    vec4 uParams1;       // x=enabled, y=intensity, z=glossyEnabled, w=reflectionIntensity
    ivec4 uParams2;      // x=glossyTilesX, yz=glossy atlas size (texels); x==0 ⇒ no sharp C1 lobe
    ivec4 uParams3;      // x=debug view (the fine block's copy is unused),
                         // y=depth atlas interior tile resolution
} DDGIVolumeFine;
layout(set = 0, binding = 35) uniform sampler2D ge_ddgiDepthAtlasFine;
layout(std430, set = 0, binding = 36) readonly buffer DDGIProbeStateFineRO
{
    vec4 ge_ddgiProbeStateFine[];
};

// Per-cascade reflection-lobe atlases — see this file's header doc,
// ddgi_rough_blend.comp and ddgi_glossy_blend.comp. Same fallback contract as
// the other atlases: C0 valid only when DDGIVolume.uParams1.z > 0.5
// (EnableGlossy); C1 when DDGIVolumeFine.uParams1.z > 0.5. The rough atlas
// shares the probe-XYZ z-major tile packing; the glossy atlas is packed
// near-square and MUST be addressed through GE_DDGIGlossyTexelUV with that
// cascade's uParams2 layout, never GE_DDGIProbeTexelUV. A cascade whose
// sharp lobe was refused (history cap) reports uParams2.x == 0 and the
// gather skips the glossy tap.
layout(set = 0, binding = 37) uniform sampler2D ge_ddgiRoughAtlas;
layout(set = 0, binding = 38) uniform sampler2D ge_ddgiGlossyAtlas;
layout(set = 0, binding = 39) uniform sampler2D ge_ddgiRoughAtlasFine;
layout(set = 0, binding = 40) uniform sampler2D ge_ddgiGlossyAtlasFine;

// Width of the soft feather band at the fine cascade's boundary, as a
// fraction of grid-space half-extent (0 = center, 1 = edge): inside
// (1-feather) the sample is pure C1, outside the boundary it is pure C0, and
// the band between blends linearly. A documented, untuned starting choice —
// a look-dev pass on real hardware would tune this.
const float GE_DDGI_FINE_CASCADE_FEATHER = 0.15;

float GE_DDGIFineCascadeWeight(vec3 posWS)
{
    vec3 fineCenter = DDGIVolumeFine.uGridMinWS.xyz + DDGIVolumeFine.uGridSizeWS.xyz * 0.5;
    vec3 fineHalfExtent = max(DDGIVolumeFine.uGridSizeWS.xyz * 0.5, vec3(1.0e-4));
    vec3 normalizedOffset = abs(posWS - fineCenter) / fineHalfExtent;
    float edgeDist = max(normalizedOffset.x, max(normalizedOffset.y, normalizedOffset.z));
    return 1.0 - clamp((edgeDist - (1.0 - GE_DDGI_FINE_CASCADE_FEATHER)) / GE_DDGI_FINE_CASCADE_FEATHER,
                       0.0, 1.0);
}

// Depth-proxy parallax correction (GE_DDGIParallaxCorrect below). The depth
// atlas's directional mean is trusted as a local surface proxy only where its
// second moment says the neighbourhood agrees: high relative variance marks a
// silhouette or a disocclusion, where a sphere fitted to the mean would bend
// the lookup onto unrelated geometry. Carried from the reference library's
// ROUGH_PARALLAX_* constants.
const float GE_DDGI_PARALLAX_VAR_START = 0.02;
const float GE_DDGI_PARALLAX_VAR_END = 0.20;
// The proxy sphere is only meaningful while it still encloses the receiver;
// fade the correction out across this fraction of its radius rather than
// letting the far quadratic root take over at the boundary.
const float GE_DDGI_PARALLAX_INSIDE_FADE = 0.12;
// Weight floor for a glossy tap whose parallax the depth proxy does not
// support. It is a RELATIVE preference, not an attenuation: every tap in the
// gather is divided by the same accumulated weight sum, so this only decides
// how much authority an unsupported probe keeps next to a supported one. With
// no supported neighbour the lobe still resolves at full strength.
const float GE_DDGI_PARALLAX_MIN_CONFIDENCE = 0.2;

// Where a probe should look to see what the receiver's reflection ray hits.
//
// Without this, eight neighbouring probes each sample their own atlas in the
// SAME world direction from eight different origins, so they disagree about
// local silhouettes and the blend reads as a smeared ghost rather than one
// reflection. The depth atlas already stores a directional mean distance per
// probe — a spherical proxy for the geometry around it — so intersecting the
// receiver's reflection ray with that sphere gives all eight a shared hit
// point to aim at.
//
// `receiverPosWS` is the UNBIASED shading position: the bias exists to keep the
// grid lookup off the surface, and letting it move the ray origin would move
// the proxy hit with it.
//
// Returns the direction to sample in `sampleDir`, and in `parallaxWeight` how
// far the correction was actually trusted (0 = the raw reflection direction was
// kept, 1 = fully corrected) — the caller's glossy confidence term.
void GE_DDGIParallaxCorrect(vec2 moments, vec3 receiverPosWS, vec3 probePosWS, vec3 reflectDir,
                            out vec3 sampleDir, out float parallaxWeight)
{
    float radius = max(moments.x, 1.0e-4);
    float radius2 = radius * radius;

    vec3 q = receiverPosWS - probePosWS;
    float q2 = dot(q, q);
    float qDotR = dot(q, reflectDir);
    float disc = radius2 - q2 + qDotR * qDotR;
    float sqrtDisc = sqrt(max(disc, 0.0));
    float nearT = -qDotR - sqrtDisc;
    float farT = -qDotR + sqrtDisc;
    float rayT = nearT > 1.0e-4 ? nearT : farT;

    // Chebyshev's second moment reused as an agreement measure: variance
    // relative to the squared mean, so it is scale-free across grid sizes.
    float relVariance = max(moments.y - radius2, 0.0) / max(radius2, 1.0e-4);
    float stableDepth = 1.0 - smoothstep(GE_DDGI_PARALLAX_VAR_START, GE_DDGI_PARALLAX_VAR_END,
                                         relVariance);
    float validHit = (disc > 1.0e-6 && rayT > 1.0e-4) ? 1.0 : 0.0;
    float insideRatio = (radius2 - q2) / max(radius2, 1.0e-4);
    float insideWeight = smoothstep(0.0, GE_DDGI_PARALLAX_INSIDE_FADE, insideRatio);

    parallaxWeight = stableDepth * validHit * insideWeight;
    vec3 correctedDir = normalize(q + reflectDir * max(rayT, 0.0));
    sampleDir = normalize(mix(reflectDir, correctedDir, parallaxWeight));
}

vec3 GE_DDGISampleC0(vec3 posWS, vec3 N)
{
    if (DDGIVolume.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolume.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolume.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolume.uProbeCount.xyz;
    grid.probeTotal = DDGIVolume.uProbeCount.w;
    grid.minCellWS = DDGIVolume.uParams0.x;
    grid.normalBiasScale = DDGIVolume.uParams0.y;

    vec3 biasedPos = posWS + GE_DDGISurfaceBias(N, grid.minCellWS, grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    vec3 numerator = vec3(0.0);
    float denom = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = base + offset;
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;

        int probeIdx = GE_DDGIProbeIndex(grid, coord);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeState[probeIdx], DDGIVolume.uParams0.w);
        if (state.w <= 0.0)
            continue;  // classified inactive (buried) at full classify strength

        vec3 probePosWS = GE_DDGIProbePosition(grid, probeIdx) + state.xyz;
        vec3 toPoint = biasedPos - probePosWS;
        float distToPoint = length(toPoint);
        vec3 towardPoint = distToPoint > 1.0e-5 ? (toPoint / distToPoint) : N;

        vec2 depthUV = GE_DDGIDepthTexelUV(grid, probeIdx, towardPoint, DDGIVolume.uParams3.y);
        vec2 moments = GE_DDGIBoundedMoments(textureLod(ge_ddgiDepthAtlas, depthUV, 0.0));
        // `towardPoint` runs probe -> shading point, so its negation is the
        // direction the wrap term wants. It is built from the biased position,
        // which differs from the true one by a fraction of a cell — invisible
        // in a term this smooth, and it saves a second normalize per corner.
        // Trilinear x wrap x visibility x classify state, the reference's
        // product with no further shaping.
        float weight = GE_DDGIBackfaceWeight(-towardPoint, N, DDGIVolume.uParams0.z) *
                       GE_DDGIVisibilityWeight(moments, distToPoint, DDGIVolume.uParams0.z,
                                               grid.minCellWS, DDGIVolume.uParams3.y);
        weight *= trilinear * state.w;

        vec2 irrUV = GE_DDGIProbeTexelUV(grid, probeIdx, N);
        numerator += textureLod(ge_ddgiIrradianceAtlas, irrUV, 0.0).rgb * weight;
        denom += weight;
    }
    if (denom <= 1.0e-4)
        return vec3(0.0);
    return (numerator / denom) * DDGIVolume.uParams1.y;
}

// Identical shape to GE_DDGISampleC0 but against the fine cascade's own
// bindings/state — see this file's header doc for why the two aren't
// shared behind one function (SSBO blocks aren't passable as GLSL params).
vec3 GE_DDGISampleC1(vec3 posWS, vec3 N)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolumeFine.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolumeFine.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolumeFine.uProbeCount.xyz;
    grid.probeTotal = DDGIVolumeFine.uProbeCount.w;
    grid.minCellWS = DDGIVolumeFine.uParams0.x;
    grid.normalBiasScale = DDGIVolumeFine.uParams0.y;

    vec3 biasedPos = posWS + GE_DDGISurfaceBias(N, grid.minCellWS, grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    vec3 numerator = vec3(0.0);
    float denom = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = base + offset;
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;

        int probeIdx = GE_DDGIProbeIndex(grid, coord);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeStateFine[probeIdx], DDGIVolumeFine.uParams0.w);
        if (state.w <= 0.0)
            continue;

        vec3 probePosWS = GE_DDGIProbePosition(grid, probeIdx) + state.xyz;
        vec3 toPoint = biasedPos - probePosWS;
        float distToPoint = length(toPoint);
        vec3 towardPoint = distToPoint > 1.0e-5 ? (toPoint / distToPoint) : N;

        vec2 depthUV = GE_DDGIDepthTexelUV(grid, probeIdx, towardPoint, DDGIVolumeFine.uParams3.y);
        vec2 moments = GE_DDGIBoundedMoments(textureLod(ge_ddgiDepthAtlasFine, depthUV, 0.0));
        float weight = GE_DDGIBackfaceWeight(-towardPoint, N, DDGIVolumeFine.uParams0.z) *
                       GE_DDGIVisibilityWeight(moments, distToPoint, DDGIVolumeFine.uParams0.z,
                                               grid.minCellWS, DDGIVolumeFine.uParams3.y);
        weight *= trilinear * state.w;

        vec2 irrUV = GE_DDGIProbeTexelUV(grid, probeIdx, N);
        numerator += textureLod(ge_ddgiIrradianceAtlasFine, irrUV, 0.0).rgb * weight;
        denom += weight;
    }
    if (denom <= 1.0e-4)
        return vec3(0.0);
    return (numerator / denom) * DDGIVolumeFine.uParams1.y;
}

// C0's reflection sample at world position `posWS`, reflection direction
// `reflectDir`, crossfading the two baked lobes by `roughLobeMix` (0 = pure
// glossy/power-64, 1 = pure rough/power-8; the consumer derives it from
// roughness, Includes/ibl.glsl).
//
// Same 8-probe trilinear + Chebyshev visibility structure as GE_DDGISampleC0
// — visibility is geometric occlusion, independent of whether the tap is a
// diffuse or a reflected radiance direction — but the atlas taps follow the
// reflection direction after depth-proxy parallax correction
// (GE_DDGIParallaxCorrect) rather than the surface normal, the per-probe
// weight is squared, and the sharp lobe additionally weights by how far that
// correction was trusted (see inline).
//
// Returns coverage-premultiplied radiance in rgb and the lobe's directional
// coverage in a (GE_DDGIRayCoverage's doc). NOT scaled by ReflectionIntensity
// here: the consumer applies it to radiance and coverage together, so turning
// the knob down returns authority to the prefiltered cube instead of fading
// toward black.
//
// Per-cascade gathers are duplicated rather than parameterized: GLSL storage
// buffers and uniform blocks are not opaque types, so C1 cannot pass
// ge_ddgiProbeStateFine into C0's loop. sampler2D can, the SSBOs cannot —
// same constraint GE_DDGISampleC0/C1 documents. GE_SampleDDGIReflectionBoth
// blends the two with GE_DDGIFineCascadeWeight.
//
// One consumer: the resolve kernel (Shaders/ddgi_glossy_resolve.comp), which
// gathers both lobes per resolve pixel. Includes/ibl.glsl never calls this
// loop: two inlined copies of it cost a forward fragment shader ~4 ms/frame
// on the Sponza parity scene in register pressure alone, whether or not a
// fragment reached them. A reduced-resolution resolve samples the reflection
// DIRECTION once per resolve texel, so on a mirror the result plateaus; a
// view that needs exact directions runs the resolve at Full scale.
void GE_DDGIGatherReflectionC0(vec3 posWS, vec3 reflectDir, bool wantRough, bool wantGlossy,
                               out vec4 roughResolved, out vec4 glossyResolved)
{
    roughResolved = vec4(0.0);
    glossyResolved = vec4(0.0);
    if (DDGIVolume.uParams1.z < 0.5)
        return;

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolume.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolume.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolume.uProbeCount.xyz;
    grid.probeTotal = DDGIVolume.uProbeCount.w;
    grid.minCellWS = DDGIVolume.uParams0.x;
    grid.normalBiasScale = DDGIVolume.uParams0.y;

    vec3 biasedPos = posWS + reflectDir * (grid.minCellWS * GE_DDGI_SURFACE_NORMAL_BIAS_CELL *
                                           grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    int glossyTilesX = DDGIVolume.uParams2.x;
    wantGlossy = wantGlossy && glossyTilesX > 0;
    vec2 glossyAtlasSize = vec2(DDGIVolume.uParams2.yz);

    vec4 roughAcc = vec4(0.0);
    float roughWsum = 0.0;
    vec4 glossyAcc = vec4(0.0);
    float glossyWsum = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = base + offset;
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;

        int probeIdx = GE_DDGIProbeIndex(grid, coord);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeState[probeIdx], DDGIVolume.uParams0.w);
        if (state.w <= 0.0)
            continue;

        vec3 probePosWS = GE_DDGIProbePosition(grid, probeIdx) + state.xyz;
        vec3 toPoint = biasedPos - probePosWS;
        float distToPoint = length(toPoint);
        vec3 towardPoint = distToPoint > 1.0e-5 ? (toPoint / distToPoint) : reflectDir;

        vec2 depthUV = GE_DDGIDepthTexelUV(grid, probeIdx, towardPoint, DDGIVolume.uParams3.y);
        vec2 moments = GE_DDGITrueDistanceMoments(textureLod(ge_ddgiDepthAtlas, depthUV, 0.0));
        float weight = trilinear * state.w * GE_DDGIVisibilityWeight(moments, distToPoint,
                                                                     DDGIVolume.uParams0.z, grid.minCellWS,
                                    DDGIVolume.uParams3.y);
        float reflectionWeight = weight * weight;

        vec2 reflectMoments = GE_DDGITrueDistanceMoments(
            textureLod(ge_ddgiDepthAtlas, GE_DDGIDepthTexelUV(grid, probeIdx, reflectDir, DDGIVolume.uParams3.y), 0.0));
        vec3 sampleDir;
        float parallaxWeight;
        GE_DDGIParallaxCorrect(reflectMoments, posWS, probePosWS, reflectDir, sampleDir,
                               parallaxWeight);

        if (wantRough)
        {
            vec2 roughUV = GE_DDGIProbeTexelUV(grid, probeIdx, sampleDir);
            roughAcc += textureLod(ge_ddgiRoughAtlas, roughUV, 0.0) * reflectionWeight;
            roughWsum += reflectionWeight;
        }
        if (wantGlossy)
        {
            float glossyWeight = reflectionWeight *
                                 mix(GE_DDGI_PARALLAX_MIN_CONFIDENCE, 1.0, parallaxWeight);
            vec2 glossyUV = GE_DDGIGlossyTexelUV(probeIdx, glossyTilesX, glossyAtlasSize, sampleDir);
            glossyAcc += textureLod(ge_ddgiGlossyAtlas, glossyUV, 0.0) * glossyWeight;
            glossyWsum += glossyWeight;
        }
    }

    roughResolved = roughAcc / max(roughWsum, 1.0e-4);
    glossyResolved = glossyAcc / max(glossyWsum, 1.0e-4);
}

void GE_DDGIGatherReflectionC1(vec3 posWS, vec3 reflectDir, bool wantRough, bool wantGlossy,
                               out vec4 roughResolved, out vec4 glossyResolved)
{
    roughResolved = vec4(0.0);
    glossyResolved = vec4(0.0);
    if (DDGIVolumeFine.uParams1.z < 0.5)
        return;

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolumeFine.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolumeFine.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolumeFine.uProbeCount.xyz;
    grid.probeTotal = DDGIVolumeFine.uProbeCount.w;
    grid.minCellWS = DDGIVolumeFine.uParams0.x;
    grid.normalBiasScale = DDGIVolumeFine.uParams0.y;

    vec3 biasedPos = posWS + reflectDir * (grid.minCellWS * GE_DDGI_SURFACE_NORMAL_BIAS_CELL *
                                           grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    int glossyTilesX = DDGIVolumeFine.uParams2.x;
    wantGlossy = wantGlossy && glossyTilesX > 0;
    vec2 glossyAtlasSize = vec2(DDGIVolumeFine.uParams2.yz);

    vec4 roughAcc = vec4(0.0);
    float roughWsum = 0.0;
    vec4 glossyAcc = vec4(0.0);
    float glossyWsum = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        ivec3 coord = base + offset;
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;

        int probeIdx = GE_DDGIProbeIndex(grid, coord);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeStateFine[probeIdx], DDGIVolumeFine.uParams0.w);
        if (state.w <= 0.0)
            continue;

        vec3 probePosWS = GE_DDGIProbePosition(grid, probeIdx) + state.xyz;
        vec3 toPoint = biasedPos - probePosWS;
        float distToPoint = length(toPoint);
        vec3 towardPoint = distToPoint > 1.0e-5 ? (toPoint / distToPoint) : reflectDir;

        vec2 depthUV = GE_DDGIDepthTexelUV(grid, probeIdx, towardPoint, DDGIVolumeFine.uParams3.y);
        vec2 moments = GE_DDGITrueDistanceMoments(textureLod(ge_ddgiDepthAtlasFine, depthUV, 0.0));
        float weight = trilinear * state.w * GE_DDGIVisibilityWeight(moments, distToPoint,
                                                                     DDGIVolumeFine.uParams0.z, grid.minCellWS,
                                    DDGIVolumeFine.uParams3.y);
        float reflectionWeight = weight * weight;

        vec2 reflectMoments = GE_DDGITrueDistanceMoments(
            textureLod(ge_ddgiDepthAtlasFine, GE_DDGIDepthTexelUV(grid, probeIdx, reflectDir, DDGIVolumeFine.uParams3.y), 0.0));
        vec3 sampleDir;
        float parallaxWeight;
        GE_DDGIParallaxCorrect(reflectMoments, posWS, probePosWS, reflectDir, sampleDir,
                               parallaxWeight);

        if (wantRough)
        {
            vec2 roughUV = GE_DDGIProbeTexelUV(grid, probeIdx, sampleDir);
            roughAcc += textureLod(ge_ddgiRoughAtlasFine, roughUV, 0.0) * reflectionWeight;
            roughWsum += reflectionWeight;
        }
        if (wantGlossy)
        {
            float glossyWeight = reflectionWeight *
                                 mix(GE_DDGI_PARALLAX_MIN_CONFIDENCE, 1.0, parallaxWeight);
            vec2 glossyUV = GE_DDGIGlossyTexelUV(probeIdx, glossyTilesX, glossyAtlasSize, sampleDir);
            glossyAcc += textureLod(ge_ddgiGlossyAtlasFine, glossyUV, 0.0) * glossyWeight;
            glossyWsum += glossyWeight;
        }
    }

    roughResolved = roughAcc / max(roughWsum, 1.0e-4);
    glossyResolved = glossyAcc / max(glossyWsum, 1.0e-4);
}

// Both lobes, un-crossfaded, from ONE gather per cascade. The scaled resolve
// (ddgi_glossy_resolve.comp) writes the two lobes to separate targets and lets
// its consumer crossfade per fragment, so gathering once per lobe would run
// the whole 8-probe loop — depth taps, visibility, parallax —
// a second time to change only which atlas is fetched. Cascade blending is
// per lobe, which is the same result: the crossfade is linear, so blending
// then mixing and mixing then blending agree.
void GE_SampleDDGIReflectionBoth(vec3 posWS, vec3 reflectDir, out vec4 roughOut, out vec4 glossyOut)
{
    float c1Weight = DDGIVolumeFine.uParams1.z < 0.5 ? 0.0 : GE_DDGIFineCascadeWeight(posWS);
    if (c1Weight >= 1.0)
    {
        GE_DDGIGatherReflectionC1(posWS, reflectDir, true, true, roughOut, glossyOut);
        return;
    }

    GE_DDGIGatherReflectionC0(posWS, reflectDir, true, true, roughOut, glossyOut);
    if (c1Weight <= 0.0)
        return;

    vec4 roughFine;
    vec4 glossyFine;
    GE_DDGIGatherReflectionC1(posWS, reflectDir, true, true, roughFine, glossyFine);
    roughOut = mix(roughOut, roughFine, c1Weight);
    glossyOut = mix(glossyOut, glossyFine, c1Weight);
}

vec3 GE_SampleDDGIIrradiance(vec3 posWS, vec3 N)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return GE_DDGISampleC0(posWS, N);

    // Weight first, gather second: at weight 1 the crossfade is the fine
    // cascade alone, so the coarse 8-probe loop is dead work. Every atlas
    // tap in these loops is textureLod, so the divergence is well-defined.
    float c1Weight = GE_DDGIFineCascadeWeight(posWS);
    if (c1Weight <= 0.0)
        return GE_DDGISampleC0(posWS, N);

    vec3 c1 = GE_DDGISampleC1(posWS, N);
    if (c1Weight >= 1.0)
        return c1;

    return mix(GE_DDGISampleC0(posWS, N), c1, c1Weight);
}

// ---------------------------------------------------------------------------
// Diagnostics (Components::DDGIDebugView)
//
// Called ONLY from the resolve compute pass (Shaders/ddgi_glossy_resolve.comp).
// Keeping them out of the forward shader is not tidiness: a gather loop costs
// a forward fragment shader its registers whether or not any fragment reaches
// it — measured at ~4 ms/frame on the Sponza parity scene — so a debug path
// compiled into the world pass would tax every frame the tool is not in use,
// and would perturb the very timings someone reaches for it to explain.
// ---------------------------------------------------------------------------

// Fraction of the trilinear cell backed by ACTIVE probes, in [0,1]. This is the
// quantity that explains probe-shaped dark patches: where it is below 1 the
// field is answering with only part of a cell, and DDGI REPLACES the
// environment sample regardless, so whatever those few probes hold is what the
// surface gets.
float GE_DDGIDebugCoverage(vec3 posWS, vec3 N)
{
    if (DDGIVolume.uParams1.x < 0.5)
        return 0.0;

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolume.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolume.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolume.uProbeCount.xyz;
    grid.probeTotal = DDGIVolume.uProbeCount.w;
    grid.minCellWS = DDGIVolume.uParams0.x;
    grid.normalBiasScale = DDGIVolume.uParams0.y;

    vec3 biasedPos = posWS + GE_DDGISurfaceBias(N, grid.minCellWS, grid.normalBiasScale);
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, biasedPos), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 base = clamp(ivec3(floor(gridSpace)), ivec3(0), max(grid.probeCount - ivec3(2), ivec3(0)));
    vec3 frac = gridSpace - vec3(base);

    float coverage = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        ivec3 offset = ivec3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        float trilinear = 1.0;
        trilinear *= (offset.x == 1) ? frac.x : (1.0 - frac.x);
        trilinear *= (offset.y == 1) ? frac.y : (1.0 - frac.y);
        trilinear *= (offset.z == 1) ? frac.z : (1.0 - frac.z);
        if (trilinear <= 0.0)
            continue;
        int probeIdx = GE_DDGIProbeIndex(grid, base + offset);
        vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeState[probeIdx], DDGIVolume.uParams0.w);
        if (state.w > 0.0)
            coverage += trilinear;
    }
    return clamp(coverage, 0.0, 1.0);
}

// Nearest probe's classification and how far relocation moved it: green when
// active, red when classified buried, brightness = |offset| as a fraction of
// the largest relocation the classify pass is allowed to apply.
vec3 GE_DDGIDebugProbeState(vec3 posWS)
{
    if (DDGIVolume.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolume.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolume.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolume.uProbeCount.xyz;
    grid.probeTotal = DDGIVolume.uProbeCount.w;
    grid.minCellWS = DDGIVolume.uParams0.x;

    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, posWS), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    int probeIdx = GE_DDGIProbeIndex(grid, ivec3(round(gridSpace)));
    vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeState[probeIdx], DDGIVolume.uParams0.w);
    // 0.45 cell is the classify pass's relocation cap (ddgi_classify.comp).
    float relocated = clamp(length(state.xyz) / max(grid.minCellWS * 0.45, 1.0e-4), 0.0, 1.0);
    float lit = mix(0.25, 1.0, relocated);
    return state.w > 0.0 ? vec3(0.0, lit, 0.0) : vec3(lit, 0.0, 0.0);
}

// The nearest coarse probe's stored irradiance toward N: one atlas texel, no
// trilinear blend, no visibility or classify weight. Scene-linear, so the
// caller shows it at scene exposure — it isolates what a probe HOLDS from how
// the gather combines probes.
vec3 GE_DDGIDebugNearestIrradiance(vec3 posWS, vec3 N)
{
    if (DDGIVolume.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolume.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolume.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolume.uProbeCount.xyz;
    grid.probeTotal = DDGIVolume.uProbeCount.w;
    grid.minCellWS = DDGIVolume.uParams0.x;

    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, posWS), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    int probeIdx = GE_DDGIProbeIndex(grid, ivec3(round(gridSpace)));
    return textureLod(ge_ddgiIrradianceAtlas, GE_DDGIProbeTexelUV(grid, probeIdx, N), 0.0).rgb *
           DDGIVolume.uParams1.y;
}

// Fine-cascade twins of the two views above, against C1's bindings.
vec3 GE_DDGIDebugProbeStateFine(vec3 posWS)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolumeFine.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolumeFine.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolumeFine.uProbeCount.xyz;
    grid.probeTotal = DDGIVolumeFine.uProbeCount.w;
    grid.minCellWS = DDGIVolumeFine.uParams0.x;

    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, posWS), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    int probeIdx = GE_DDGIProbeIndex(grid, ivec3(round(gridSpace)));
    vec4 state = GE_DDGIProbeStateApply(ge_ddgiProbeStateFine[probeIdx], DDGIVolumeFine.uParams0.w);
    float relocated = clamp(length(state.xyz) / max(grid.minCellWS * 0.45, 1.0e-4), 0.0, 1.0);
    float lit = mix(0.25, 1.0, relocated);
    return state.w > 0.0 ? vec3(0.0, lit, 0.0) : vec3(lit, 0.0, 0.0);
}

vec3 GE_DDGIDebugNearestIrradianceFine(vec3 posWS, vec3 N)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return vec3(0.0);

    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolumeFine.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolumeFine.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolumeFine.uProbeCount.xyz;
    grid.probeTotal = DDGIVolumeFine.uProbeCount.w;
    grid.minCellWS = DDGIVolumeFine.uParams0.x;

    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, posWS), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    int probeIdx = GE_DDGIProbeIndex(grid, ivec3(round(gridSpace)));
    return textureLod(ge_ddgiIrradianceAtlasFine, GE_DDGIProbeTexelUV(grid, probeIdx, N), 0.0).rgb *
           DDGIVolumeFine.uParams1.y;
}

// Fine grid coordinate (mode 0) or storage slot (mode 1) of the nearest
// probe, as xyz / resolution.
vec3 GE_DDGIDebugFineProbeAddress(vec3 posWS, int mode)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return vec3(0.0);
    GE_DDGIGridInfo grid;
    grid.gridMinWS = DDGIVolumeFine.uGridMinWS.xyz;
    grid.gridSizeWS = DDGIVolumeFine.uGridSizeWS.xyz;
    grid.probeCount = DDGIVolumeFine.uProbeCount.xyz;
    grid.probeTotal = DDGIVolumeFine.uProbeCount.w;
    grid.minCellWS = DDGIVolumeFine.uParams0.x;
    vec3 gridSpace = clamp(GE_DDGIWorldToGridSpace(grid, posWS), vec3(0.0),
                           vec3(grid.probeCount - ivec3(1)));
    ivec3 coord = ivec3(round(gridSpace));
    int probeIdx = GE_DDGIProbeIndex(grid, coord);
    int resX = max(grid.probeCount.x, 1);
    int resY = max(grid.probeCount.y, 1);
    ivec3 slot = ivec3(probeIdx % resX, (probeIdx / resX) % resY, probeIdx / (resX * resY));
    return vec3(mode == 0 ? coord : slot) / vec3(max(grid.probeCount, ivec3(1)));
}

// Which grid owns this point: red = coarse only, green = fine, yellow between.
vec3 GE_DDGIDebugCascadeWeight(vec3 posWS)
{
    if (DDGIVolumeFine.uParams1.x < 0.5)
        return vec3(1.0, 0.0, 0.0);
    float w = clamp(GE_DDGIFineCascadeWeight(posWS), 0.0, 1.0);
    return vec3(1.0 - w, w, 0.0);
}

#endif // GE_DDGI_RESOLVE_ONLY

#endif // GE_DDGI_PROBES_GLSL
