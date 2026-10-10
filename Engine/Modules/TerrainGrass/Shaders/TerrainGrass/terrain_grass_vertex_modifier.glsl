// Procedural terrain grass blade vertex modifier.
// Uses TerrainGPUParams + splatmap red channel (terrain grass layer) to place
// tapered blades only where the terrain material says grass is present.

#extension GL_EXT_nonuniform_qualifier : require

// Procedural grass has no tangent vertex attribute (VertexFlags::None), so reuse the
// adapter's unused tangent location. Keep locations below 16 for WebGPU compatibility.
layout(location = 3) flat out float vGrassWindStrength;

layout(location = 0) in vec2 aBlade; // x = side -1..1, y = height fraction 0..1

struct TerrainParamsEntry
{
    float WorldOriginX;
    float WorldOriginZ;
    float WorldSizeX;
    float WorldSizeZ;
    float HeightScale;
    float InvHeightmapWidth;
    float InvHeightmapHeight;
    float TexelSize;
    float WorldOriginY;
    uint SplatmapBindless;
    float MaterialTiling;
    uint LayerCount;
    uint LayerRole[4];
    float GridDimHalf;
    uint Flags;
    uint NormalmapBindless;
    uint HeightmapBindless;
    uint GrassEnabled;
    float GrassDensity;
    float GrassBladeHeight;
    float GrassBladeWidth;
    float GrassMaskThreshold;
    float GrassDensityFalloff;
    float GrassRange;
    float GrassWindStrength;
    uint GrassLayerIndex;
    float GrassBrightness;
    float GrassGroundingStrength;
    float GrassRandomScale;
    float GrassRandomBrightness;
    float GrassTranslucency;
    uint GrassRootColor;
    uint GrassTipColor;
    uint GrassBacklightColor;
    float GrassWindDirection;
    float GrassWindGustSpeed;
    float GrassWindGustScale;
    float GrassWindRestingLean;
    float GrassWindFlutterAmount;
    float GrassWindFlutterSpeed;
    float GrassWindSeed;
    uint GrassBladeSegments;
    uint GrassAlbedoBindless;
    uint GrassAlphaBindless;
    uint GrassNormalBindless;
    uint GrassAtlasColumns;
    uint GrassAtlasRows;
    uint GrassAtlasTileCount;
    float GrassAlphaCutoff;
    float GrassNormalStrength;
    float GrassTextureCardsPerSquareMeter;
    float GrassTextureSize;
    float GrassPlacementSeed;
    float GrassMaxWidthRatio;
    float GrassClumpSize;
    float GrassClumpHeightVariance;
    float GrassClumpAlignment;
    float GrassClumpGather;
    float GrassHueVariation;
    float GrassRootShade;
    float GrassBladeNormalForm;
    float GrassBladeScatterGain;
    float GrassRootFadeStart;
    float GrassRootFadeEnd;
    uint GrassControlBindless;
};

layout(std430, set = 2, binding = 1) readonly buffer TerrainParamsBuffer
{
    TerrainParamsEntry terrains[];
};

struct GrassBladeInstance
{
    float WorldX;
    float WorldZ;
    float RootY;
    float BladeHeight;
    float BladeWidth;
    float Yaw;
    float Rand;
    float GrassWeight;
    uint TerrainIndex;
    // Terrain surface normal baked by the placement compute for atlas terrains (world nx/nz; ny
    // reconstructed). 0 for non-atlas terrains, which re-sample the normal below (path unchanged).
    float NormalX;
    float NormalZ;
    uint Flags; // LOD index + clump colour code; grass_clump.glsl owns the bit layout
};

// TerrainGPUParams::Flags bit1 (mirror Terrain::kTerrainFlagAtlasBacked): height/normal come from
// the atlas, not a unified bindless heightmap/normalmap (both bindless indices are 0 on this path).
const uint kTerrainFlagAtlasBacked = 2u;

layout(std430, set = 2, binding = 3) readonly buffer TerrainGrassInstances
{
    GrassBladeInstance grassInstances[];
};

// The per-view budget fit shortens the AUTHORED range, and the placement compute spawns against
// the shortened one. This is the draw's copy of that scale so the distance band below can be built
// on the same radius: keyed to the authored range instead, the band is still mid-ramp where the
// last blades stand and the blade-to-ground handover never completes, which reads as a hard edge
// at the field boundary. Carries an INSTANCE name, so reflection reports it as `GrassDraw`.
layout(std140, set = 2, binding = 7) uniform GrassDrawParamsBuffer
{
    float RangeFitScale; // plan.Params.FarRadius / summary.MaxRange; 1 = the fit changed nothing
    float _Pad0;
    float _Pad1;
    float _Pad2;
} GrassDraw;

// Full profile only: this stage's taps go through GE_BTEX, and the compat half of that header
// declares a biased sampler helper, which GLSL does not allow in a vertex shader (no implicit
// LOD here). The compat arm below reads named bindings instead and needs nothing from it.
#if !defined(GE_COMPAT_PROFILE)
#include "Includes/bindless_textures.glsl"
#endif
#include "TerrainGrass/grass_clump.glsl"
#include "TerrainGrass/grass_wind_volumes.glsl"
// CBT_LatticeTextureUV: where the unified height and normal maps are read for a terrain UV.
#include "CBT/cbt_atlas.glsl"

#if defined(GE_COMPAT_PROFILE)
// WebGPU exposes no binding array; the two data maps this stage reads are named set-2 bindings.
// Bindings continue the surface's block (20-25) so one layout serves the whole material.
// Separate texture + sampler, sharing the surface's sampler at binding 28 (one binding, both
// stages) — see the surface for why a combined sampler2D is not affordable here.
layout(set = 2, binding = 26) uniform texture2D gGrassVtxHeightmap;
layout(set = 2, binding = 27) uniform texture2D gGrassVtxNormalmap;
layout(set = 2, binding = 28) uniform sampler gGrassMapSampler;
#endif

float hash11(float p)
{
    p = fract(p * 0.1031);
    p *= p + 33.33;
    p *= p + p;
    return fract(p);
}

vec3 permute3(vec3 x)
{
    return mod(((x * 34.0) + 1.0) * x, 289.0);
}

float snoise(vec2 v)
{
    const vec4 C = vec4(0.211324865405187, 0.366025403784439,
                       -0.577350269189626, 0.024390243902439);
    vec2 i = floor(v + dot(v, C.yy));
    vec2 x0 = v - i + dot(i, C.xx);
    vec2 i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
    vec4 x12 = x0.xyxy + C.xxzz;
    x12.xy -= i1;

    i = mod(i, 289.0);
    vec3 p = permute3(permute3(i.y + vec3(0.0, i1.y, 1.0))
        + i.x + vec3(0.0, i1.x, 1.0));

    vec3 m = max(0.5 - vec3(dot(x0, x0), dot(x12.xy, x12.xy),
        dot(x12.zw, x12.zw)), 0.0);
    m = m * m;
    m = m * m;

    vec3 x = 2.0 * fract(p * C.www) - 1.0;
    vec3 h = abs(x) - 0.5;
    vec3 ox = floor(x + 0.5);
    vec3 a0 = x - ox;
    m *= 1.79284291400159 - 0.85373472095314 * (a0 * a0 + h * h);

    vec3 g;
    g.x = a0.x * x0.x + h.x * x0.y;
    g.yz = a0.yz * x12.xz + h.yz * x12.yw;
    return 130.0 * dot(m, g);
}

const mat2 kWindRot = mat2(0.86, 0.50, -0.50, 0.86) * 1.93;

// Mirror of kGrassLod1RadiusFraction in TerrainGrass/GrassPlacementModel.h: LOD 1 begins at this
// fraction of the FITTED range, and the surface's far-LOD colour blend ramps from there to that
// same range. Fitted, not authored, on both sides: the CPU derives GrassPlacementPlan::
// Lod1StartDistance from the fitted radius and hands it to the compute as Place.Bounds.z, so a band
// built on the authored radius would start the colour blend somewhere the geometry LOD does not.
const float kGrassLod1RadiusFraction = 0.18;
// Width taper along the blade, pow(1 - t, e). Below 1 the profile is concave, so the blade holds
// its width through the lower half and narrows late instead of thinning immediately off the root.
const float kGrassBladeTaperExponent = 0.85;
// The root sinks slightly INTO the terrain along its normal rather than standing on it: a blade
// that starts exactly at the surface still shows a lit sliver of its own base through the ground
// silhouette at grazing angles. Negative = embed.
const float kGrassRootEmbed = -0.01;

// The authored per-terrain wind seed as a noise-domain offset, in the SCALED domain each field
// samples in. Every noise field in the wind path takes it, so two terrains authored with different
// seeds get uncorrelated gusts, cross-deflection and flutter instead of one field in common. A
// field that skipped it would be byte-identical on every terrain and quietly pin the others to it.
// Seed 0 is the unoffset field.
// The literals carry the `f` suffix because this block is lifted verbatim into the host tests: an
// unsuffixed literal is a float in GLSL but a double in C++, where it narrows and MSVC's C4244
// turns it into a build error (GlslShim.h).
// GE_SHARED_WIND_SEED_OFFSET_BEGIN
vec2 windSeedOffset(float seed)
{
    return vec2(seed * 31.7f, seed * -17.3f);
}
// GE_SHARED_WIND_SEED_OFFSET_END

float sfbm(vec2 p)
{
    uint terrainIdx = grassInstances[uint(gl_InstanceIndex)].TerrainIndex;
    TerrainParamsEntry tp = terrains[terrainIdx];
    p += windSeedOffset(tp.GrassWindSeed);
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        v += a * snoise(p);
        p = kWindRot * p;
        a *= 0.5;
    }
    return v * 0.5 + 0.5;
}

struct WindField
{
    vec2 dir;
    float strength;
    float detail;
};

// Cross-wind deflection of the authored direction by the field's cross noise. `flow` advects the
// noise, so vec2(0) samples the unadvected field — the flowing variant's flow == 0 slice. It
// varies with position and never with time; at any other instant the flowing field has translated
// away from it.
vec2 windDeflectedDir(vec2 dir, vec2 p, float gustScale, vec2 flow, float seed)
{
    float cross = snoise(p * gustScale * 1.7 - flow * 0.85 + vec2(-31.0, 23.0)
                         + windSeedOffset(seed));
    vec2 crossDir = vec2(-dir.y, dir.x);
    return normalize(dir + crossDir * (cross * 0.55));
}

// timeSeconds is the endpoint's animation clock (inst.deformationTimeSeconds), whose per-frame
// advance is capped (Time.cpp). Wall time must never drive wind: a stalled frame loop would resume
// by rendering the whole elapsed phase gap in a single frame, and a captured frame's wind would
// not be part of the capture.
WindField sampleWindField(TerrainParamsEntry tp, vec2 p, float timeSeconds)
{
    vec2 dir = vec2(cos(tp.GrassWindDirection), sin(tp.GrassWindDirection));
    vec2 flow = dir * (timeSeconds * tp.GrassWindGustSpeed);

    float gustScale = max(tp.GrassWindGustScale, 0.0001);
    float broad = sfbm(p * gustScale - flow);
    float detail = snoise(p * gustScale * 3.1 - flow * 1.7 + vec2(17.0, -9.0)
                          + windSeedOffset(tp.GrassWindSeed));

    WindField w;
    w.dir = windDeflectedDir(dir, p, gustScale, flow, tp.GrassWindSeed);
    w.strength = clamp(0.28 + broad * 0.62 + detail * 0.10, 0.0, 1.0);
    w.detail = detail;
    return w;
}

// The RESTING lean direction: the authored direction deflected by the same cross noise the wind
// field uses, sampled UNADVECTED (flow = 0), so it varies per position but never per frame. The
// resting pose must not ride the animated field: with WindStrength authored to zero the field
// still flows, and a static-magnitude lean along a flowing direction is frame-to-frame churn on
// every blade (the ~11%-of-pixels shimmer this replaced).
vec2 restingLeanDir(TerrainParamsEntry tp, vec2 p)
{
    vec2 dir = vec2(cos(tp.GrassWindDirection), sin(tp.GrassWindDirection));
    return windDeflectedDir(dir, p, max(tp.GrassWindGustScale, 0.0001), vec2(0.0),
                            tp.GrassWindSeed);
}

// A unified height or normal map read at a terrain UV: those maps are terrain lattices
// (CBT_LatticeTextureUV), so the blade's root follows the surface the terrain draws.
#if defined(GE_COMPAT_PROFILE)
#define GRASS_VTX_LATTICE_TAP(mapIndex, mapTex, uv) \
    textureLod(sampler2D(mapTex, gGrassMapSampler), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(sampler2D(mapTex, gGrassMapSampler), 0))), 0.0)
#else
#define GRASS_VTX_LATTICE_TAP(mapIndex, mapTex, uv) \
    textureLod(GE_BTEX(mapIndex, GE_TS_CLAMP), \
               CBT_LatticeTextureUV(uv, vec2(textureSize(GE_BTEX(mapIndex, GE_TS_CLAMP), 0))), 0.0)
#endif

float sampleHeight01(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.HeightmapBindless == 0u)
        return 0.0;
    return GRASS_VTX_LATTICE_TAP(tp.HeightmapBindless, gGrassVtxHeightmap, uv).r;
}

vec3 sampleTerrainNormalWS(TerrainParamsEntry tp, vec2 uv)
{
    if (tp.NormalmapBindless != 0u)
    {
        vec2 nXZ = GRASS_VTX_LATTICE_TAP(tp.NormalmapBindless, gGrassVtxNormalmap, uv).rg;
        float ny = sqrt(max(1.0 - dot(nXZ, nXZ), 0.0));
        return normalize(vec3(nXZ.x, ny, nXZ.y));
    }

    if (tp.HeightmapBindless == 0u)
        return vec3(0.0, 1.0, 0.0);

    float texelSize = tp.TexelSize;
    float hL = sampleHeight01(tp, uv + vec2(-texelSize, 0.0));
    float hR = sampleHeight01(tp, uv + vec2( texelSize, 0.0));
    float hD = sampleHeight01(tp, uv + vec2(0.0, -texelSize));
    float hU = sampleHeight01(tp, uv + vec2(0.0,  texelSize));
    float spacingX = max(tp.WorldSizeX * texelSize * 2.0, 0.0001);
    float normalScaleX = tp.HeightScale / spacingX;
    return normalize(vec3(
        (hL - hR) * normalScaleX,
        1.0,
        (hD - hU) * normalScaleX
    ));
}

vec3 tangentPlaneDir(vec3 dir, vec3 normalWS)
{
    vec3 projected = dir - normalWS * dot(dir, normalWS);
    float len2 = dot(projected, projected);
    if (len2 > 0.0001)
        return projected * inversesqrt(len2);
    return normalize(cross(abs(normalWS.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), normalWS));
}

void ModifyVertex(inout VertexOutput v, InstanceData inst)
{
    uint instanceIdx = uint(gl_InstanceIndex);
    GrassBladeInstance blade = grassInstances[instanceIdx];
    uint terrainIdx = blade.TerrainIndex;
    TerrainParamsEntry tp = terrains[terrainIdx];

    float worldX = blade.WorldX;
    float worldZ = blade.WorldZ;
    vec2 terrainUV = clamp(vec2(
        (worldX - tp.WorldOriginX) / max(tp.WorldSizeX, 0.0001),
        (worldZ - tp.WorldOriginZ) / max(tp.WorldSizeZ, 0.0001)
    ), vec2(0.0), vec2(1.0));
    // Atlas terrains have no unified heightmap/normalmap (both bindless indices are 0): the
    // placement compute already snapped RootY through the atlas and baked the surface normal into
    // the instance, so use those directly. Non-atlas terrains re-sample exactly as before.
    bool atlasBacked = (tp.Flags & kTerrainFlagAtlasBacked) != 0u;
    float rootY = blade.RootY;
    if (tp.HeightmapBindless != 0u)
        rootY = tp.WorldOriginY + sampleHeight01(tp, terrainUV) * tp.HeightScale;
    vec3 terrainNormal;
    if (atlasBacked)
    {
        float ny = sqrt(max(1.0 - blade.NormalX * blade.NormalX - blade.NormalZ * blade.NormalZ, 0.0));
        terrainNormal = normalize(vec3(blade.NormalX, ny, blade.NormalZ));
    }
    else
    {
        terrainNormal = sampleTerrainNormalWS(tp, terrainUV);
    }
    float r = blade.Rand;
    float grassWeight = blade.GrassWeight;
    vec3 sideDir = tangentPlaneDir(vec3(cos(blade.Yaw), 0.0, sin(blade.Yaw)), terrainNormal);

    tp = grassApplyWindVolumes(tp, vec3(worldX, rootY, worldZ));
    // Every tier samples the animated field: the wind is what makes a meadow read as a meadow, and
    // a distance band that holds a constant lean instead reads as a painted backdrop behind the
    // grass that moves. The field is sampled at the blade ROOT, so it is one value per blade and
    // the far tier's reduced blade already pays it over half as many vertices. Volumes modify the
    // terrain entry first so direction, gust, and lean all see the same resolved wind.
    WindField wind = sampleWindField(tp, vec2(worldX, worldZ), inst.deformationTimeSeconds);
    vec2 leanDirStatic = restingLeanDir(tp, vec2(worldX, worldZ));

    float bladeHeight = max(0.0, blade.BladeHeight);
    float bladeWidth = max(0.0, blade.BladeWidth);
    // World width this blade may not project narrower than, sized once per blade by the placement
    // compute and carried as a multiple of the width above. Taken from the AUTHORED width, before
    // the aspect cap and the textured-card halving below, because the floor is a screen-space
    // quantity: it is the same number of world metres whichever of those two the blade goes on to
    // take. 0 = no floor.
    float minWidth = bladeWidth * GrassMinWidthScaleFromFlags(blade.Flags);
    float t = clamp(aBlade.y, 0.0, 1.0);
    bool texturedCard = (tp.GrassEnabled & 4u) != 0u;
    if (texturedCard)
    {
        bladeHeight = max(bladeWidth, 0.001);
        bladeWidth *= 0.5;
    }
    else
        bladeWidth = min(bladeWidth, bladeHeight * max(0.0, tp.GrassMaxWidthRatio));
    float width = texturedCard ? bladeWidth : bladeWidth * pow(1.0 - t, kGrassBladeTaperExponent);
    float flutterPhase =
        snoise(vec2(worldX, worldZ) * 0.37 + windSeedOffset(tp.GrassWindSeed)
               + inst.deformationTimeSeconds * max(0.0, tp.GrassWindFlutterSpeed));
    float windStrength = max(0.0, tp.GrassWindStrength) * 0.45;
    // Flutter rides the ANIMATED field direction, so it must carry the authored strength or a
    // meadow at WindStrength 0 still animates at the component's non-zero default FlutterAmount.
    // Same gate as the surface's wind shading: identity at strength >= 1, zero at 0.
    float windGate = clamp(tp.GrassWindStrength, 0.0, 1.0);
    vGrassWindStrength = windGate;
    // Per-blade ±4% lean variation from data that is a pure function of the blade's site.
    // NEVER from gl_InstanceIndex: the compacted slot a blade lands in is reallocated by a
    // subgroup atomicAdd every frame, so anything derived from it re-rolls per frame and reads
    // as field-wide jitter on a static camera.
    float bladeLean = mix(0.96, 1.04, hash11(r + blade.Yaw * 0.37));
    // Lean composed as a VECTOR: the resting pose along the static per-site direction, the wind
    // and flutter response along the animated field direction. A scalar sum applied along the
    // animated direction would swing the resting pose with the field and double-count the gusts.
    // Zero resolved wind keeps grass still, including inside calm volumes.
    vec2 leanVec = leanDirStatic * tp.GrassWindRestingLean
        + wind.dir * (wind.strength * windStrength
                      + tp.GrassWindFlutterAmount * windGate * (wind.detail + flutterPhase * 0.35));
    float leanMag = length(leanVec);
    vec2 leanDir2 = leanMag > 0.0001 ? leanVec / leanMag : leanDirStatic;
    vec3 bendDir = tangentPlaneDir(vec3(leanDir2.x, 0.0, leanDir2.y), terrainNormal);
    // The sign of a backward gust lives in the direction now, so the magnitude clamp keeps only
    // the old upper bound.
    float bend = min(leanMag * bladeLean, 1.15);
    float lean = bend * t * t;
    float ct = cos(lean);
    float st = sin(lean);

    vec3 root = vec3(worldX, rootY, worldZ) + terrainNormal * kGrassRootEmbed;
    vec3 spine = root + terrainNormal * (ct * t * bladeHeight) + bendDir * (st * t * bladeHeight);
    // Hold the blade to a minimum projected width. The floor is applied to the TAPERED width, per
    // vertex, so it lifts exactly the part of the blade that has gone sub-pixel; the expansion
    // ceiling is a multiple of that same width, which keeps the tip a point (zero times anything
    // is zero) instead of blunting every blade into a rectangle.
    //
    // Needs no guard. Where the blade already exceeds the floor — and where there is no floor at
    // all, minWidth 0 — the lower bound loses and clamp returns this vertex's own `width` operand
    // unchanged, so those blades are bit-identical to not having the feature.
    width = clamp(minWidth, width, width * kGrassMaxWidthExpansion);
    vec3 pos = spine + sideDir * (aBlade.x * width);

    // Distance LOD term the surface reads: 1 at the camera, 0 at the range placement actually
    // spawned to. It drives the blade-to-ground COLOUR blend, which keeps the far field from
    // reading as noise. It is NOT an opacity fade — the surface no longer routes it into alpha —
    // because blades leave by not being spawned and the placement compute scales the outermost
    // slots to nothing. The band is derived from the range rather than being its own knob, so there
    // is one distance control on this component, not two that can disagree — and it is the FITTED
    // range, the same one the compute used, not the authored ceiling.
    float dist = distance(Cam.uCameraPos.xz, root.xz);
    float fieldRange = max(tp.GrassRange * GrassDraw.RangeFitScale, 1.0);
    float lodStart = fieldRange * kGrassLod1RadiusFraction;
    float farLodTerm = 1.0 - smoothstep(lodStart, max(lodStart + 1.0, fieldRange), dist);

    v.position = pos;
    vec3 tangent = normalize(terrainNormal * ct + bendDir * st);
    v.normal = normalize(cross(sideDir, tangent) + sideDir * (aBlade.x * 0.56));
    v.uv0 = vec2(t, aBlade.x * 0.5 + 0.5);
    // Three of the four components carry packed payloads — grass_clump.glsl owns the whole
    // layout and states why each field is sized the way it is.
    //
    // The terrain normal goes through here rather than being resampled in the fragment stage
    // because this stage can resolve it on every terrain and that one cannot: the fragment side
    // has no heightmap-gradient fallback, so on a heightmap-only terrain it answers world-up and
    // would reintroduce exactly the base-versus-ground mismatch the settle exists to remove. It is
    // also the FOOT's normal that the surface wants, which is constant along a blade; a fragment
    // resample would drift with the bend.
    v.custom0 = vec4(GrassPackFarLodAndGust(farLodTerm, wind.strength),
                     r,
                     GrassPackTerrainNormal(terrainNormal),
                     GrassPackTerrainAndClump(terrainIdx, GrassClumpCodeFromFlags(blade.Flags)));
}
