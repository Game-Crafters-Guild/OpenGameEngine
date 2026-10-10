// Shared ocean parameters + FFT ocean sampling helpers.
// Included by both ocean_vertex_modifier.glsl (vertex stage) and
// ocean_surface.glsl (fragment stage). Declares the set-2 OceanParamsBuffer the
// OceanForwardContributor binds by name, so the layout is defined in one place.
//
// The std430 block layout MUST match OceanParamsGPU in Ocean/OceanTypes.h.

#ifndef GE_OCEAN_COMMON_GLSL
#define GE_OCEAN_COMMON_GLSL

#include "Ocean/ocean_cascade_common.glsl"

const float GE_OCEAN_GRAVITY = 9.81;
const int   GE_OCEAN_MAX_GERSTNER = 16;

// FFT cascade count — must match GE_FFT_CASCADES (ocean_fft_common.glsl) and
// CASCADE_COUNT in C++. Each cascade c is a tileable 0.5*2^c m displacement patch.
const int   GE_OCEAN_FFT_CASCADES = 16;

// Legacy analytic wave layout. Kept in the SSBO tail for binary compatibility;
// the runtime ocean shape is FFT-driven.
struct GerstnerWaveGPU
{
    float DirectionX;
    float DirectionZ;
    float Amplitude;
    float Wavelength;
    float Steepness;
    float Speed;
    float Pad0;
    float Pad1;
};

// Set 2, binding 1 — bound by name "OceanParamsBuffer" via DrawBindings.
layout(std430, set = 2, binding = 1) readonly buffer OceanParamsBuffer
{
    float uTime;
    float uSeaLevel;
    float uPatchExtent;
    uint  uGerstnerWaveCount;
    vec4  _uDeadShallowColor;  // dead: superseded by uSubSurfaceShallowCol (kept for layout)
    vec4  uDeepColor;
    float uChoppyScale;
    float uFresnelPower;
    uint  uWaveMode;          // legacy serialized mode; runtime shape uses FFT cascades
    uint  uFFTCascadeCount;   // active FFT cascades (0 until the FFT runs)
    vec4  _uDeadSkyColor;      // dead: superseded by uSkyBase/uSkyTowardsSun/uSkyAwayFromSun (kept for layout)
    vec4  uFoamColor;         // whitecap color
    float uReflectionStrength; // sky/planar reflection amount (scales the Fresnel blend)
    float uSubsurfaceStrength;
    float uFoamAmount;
    float _uDeadSmoothness;    // dead: superseded by uRoughness (kept for layout)
    vec4  uSunDirection;      // xyz toward the sun (normalized)
    vec4  uSunColor;          // directional light colour * intensity
    // Foam controls — mirror of OceanParamsGPU's foam block (8 floats, two vec4
    // lanes) inserted BEFORE the runtime-sized uWaves[] tail.
    float uFoamFadeRate;
    float uWaveFoamStrength;
    float uWaveFoamCoverage;
    float uFoamScale;
    float uFoamFeather;
    float uIntersectionFoamDepth;
    float uIntersectionFoamStrength;
    float uFoamPad2;
    // --- Material / shading parity block (mirror of OceanParamsGPU). Order +
    // std430 padding must match byte-for-byte: 9 vec4 lanes then 4 scalar lanes.
    vec4  uDiffuse;             // view-up diffuse scatter tint
    vec4  uDiffuseGrazing;      // grazing-angle diffuse scatter tint
    vec4  uDiffuseShadow;       // in-shadow diffuse tint
    vec4  uSubSurfaceShallowCol;// shallow-water shoreline tint
    vec4  uSubSurfaceColour;    // sun-driven subsurface scatter colour
    vec4  uSkyBase;             // procedural sky zenith/base
    vec4  uSkyTowardsSun;       // sky tint toward the sun
    vec4  uSkyAwayFromSun;      // sky tint away from the sun
    vec4  uDirectionalLightColor; // ocean-only tint for direct sun glitter
    float uNormalsStrength;
    float uNormalsScale;
    float uSubSurfaceDepthMax;
    float uSubSurfaceDepthPower;
    float uSubSurfaceBase;
    float uSubSurfaceSun;
    float uSubSurfaceSunFallOff;
    float uSpecular;
    float uRoughness;
    float uIorAir;
    float uIorWater;
    uint  uPlanarReflections;
    float uSkyDirectionality;
    float uDirectionalLightBoost;
    float uDirectionalLightFallOff;
    float uPlanarReflectionStrength;
    // Refraction / depth-fog (Phase 4). Lane A: xyz = per-channel depth-fog
    // extinction; w = screen-space refraction distortion strength. Lane B:
    // x = grab-available flag (1 = sample the scene grab; 0 = opaque),
    // y = grab depth-matched flag, then the shallow clarity window (meters of
    // view-ray path, and the fog scale at the surface; see OceanShallowFogScale).
    // Lane C: x = falloff start distance, y = end distance (0 = auto),
    // z = falloff power, w = mode (0 exponential, 1 linear, 2 smooth).
    vec4  uDepthFogRefraction; // rgb = DepthFogDensity, a = RefractionStrength
    uvec2 uRefractionFlags;    // x = RefractionAvailable, y = RefractionDepthMatched
    float uShallowClarityDistance;
    float uShallowClarityFloor;
    vec4  uDepthFogFalloff;
    // Sea-floor depth cascade (Phase 5). Mirror of OceanParamsGPU's shoreline
    // lane. Keep the availability flag unsigned, matching the CPU upload;
    // interpreting its 1u bits as a float would silently disable depth sampling.
    float uShorelineFoamMaxDepth;
    float uShorelineFoamStrength;
    uint  uSeabedDepthAvailable;
    float uSeabedPad0;
    // Underwater caustics (Phase 6). Mirror of OceanParamsGPU's two caustics
    // lanes (8 scalars), order + std430 padding byte-for-byte. Lane 1: scale,
    // average, strength, focal depth. Lane 2: depth-of-field, distortion
    // strength, distortion scale, then the availability gate (uint, stamped by
    // the contributor at emit; 0 = skip caustics).
    float uCausticsScale;
    float uCausticsAverage;
    float uCausticsStrength;
    float uCausticsFocalDepth;
    float uCausticsDepthOfField;
    float uCausticsDistortionStrength;
    float uCausticsDistortionScale;
    uint  uCausticsAvailable;
    // Underwater rendering (Phase 7). Mirror of OceanParamsGPU's underwater lane:
    // x = Underwater (1 = camera submerged this frame + back-face/SSS-from-below),
    // y = MeniscusWidth (screen-height fraction of the waterline band), then two
    // pads filling the std430 16-byte lane.
    uint  uUnderwater;
    float uMeniscusWidth;
    float uUnderwaterPad0;
    float uUnderwaterPad1;
    // Flow + dynamic waves (Phase 8 + 9). Mirror of OceanParamsGPU's flow/dynwave
    // lane: x = FlowAvailable (1 = sample the flow cascade for detail-UV scroll),
    // y = DynamicWavesAvailable (1 = add the dynamic-wave height + normal fold),
    // z = DynWavesAmplitude (vertical scale of the dynamic-wave height),
    // w = FlowDetailScale (multiplier on flow*time for the detail-UV scroll).
    uint  uFlowAvailable;
    uint  uDynamicWavesAvailable;
    float uDynWavesAmplitude;
    float uFlowDetailScale;
    float uDynWavesHorizontalDisplacement;
    float uDynWavesDisplacementClamp;
    float uDynWavesPad0;
    float uDynWavesPad1;
    // Clip surface + albedo (Phase 10 + 11). Mirror of OceanParamsGPU's clip/albedo
    // lane: x = ClipAvailable (1 = sample the clip cascade + discard where clipped),
    // y = DefaultClippingState (the clip value outside every source; matches the
    // bake so the gate is consistent), z = AlbedoAvailable (1 = blend the albedo
    // cascade into baseColor before lighting), w = NormalTextureAvailable.
    uint  uClipAvailable;
    float uDefaultClippingState;
    uint  uAlbedoAvailable;
    uint  uNormalTextureAvailable;
    // Texture overrides + planar reflection gates. Mirror of OceanParamsGPU's
    // texture-override lane: x = FoamTextureAvailable (sample uOceanFoamBubble
    // instead of analytic Worley), y = CausticsTextureAvailable (user caustics image
    // -> skip the encoded-normal distortion), z = PlanarReflectionAvailable (sample
    // uOceanReflection instead of the procedural sky dome), w = shallow-refraction
    // reflection suppression.
    uint  uFoamTextureAvailable;
    uint  uCausticsTextureAvailable;
    uint  uPlanarReflectionAvailable;
    float uShallowRefractionReflectionSuppression;
    // Displacement combine cascade gate (mirror of OceanParamsGPU). 1 = sample the
    // combined cascade (~2 slices) instead of summing all 16 FFT cascades.
    uint  uCombineWavesAvailable;
    uint  uFoamDebugMode;
    uint  uWaveMaskAvailable;
    uint  uLocalFFTAvailable;
    // Shape FFT wave-shape controls (mirror of OceanParamsGPU's lane). Applied to the
    // sampled FFT displacement by OceanShapeDisplacement below.
    float uWeight;
    float uMaxHorizontalDisplacement;
    float uMaxVerticalDisplacement;
    float uRespectShallowAttenuation;
    // Surface geometry lane. uGeometryGridSize is the compatible budget mapped
    // to each concentric LOD tile's cell count; the vertex shader uses the cell
    // width for displacement footprint filtering so denser tiles reveal smaller
    // near-field waves.
    float uGeometryGridSize;
    float uWaveOriginOffsetX;
    float uWaveOriginOffsetZ;
    float uGeometryPad0;
    // Foam material detail, mirror of OceanParamsGPU.
    float uFoamNormalStrength;
    float uFoamBubbleCoverage;
    float uFoamBubbleParallax;
    float uFoamRoughness;
    GerstnerWaveGPU uWaves[];
};

// FFT displacement cascade array: layer c is a tileable 0.5*2^c m patch holding
// (displaceX, height, displaceZ). Bound by name on the ocean draw; sampled with
// Repeat wrap + Linear filter. Vertex stage must use textureLod (no derivatives).
layout(set = 2, binding = 2) uniform sampler2DArray uOceanDisplacement;

// Simulated foam cascade (RGBA16F): per-LOD camera-snapped foam state built by
// ocean_foam_sim.comp. R = wave/breaking foam, G = shoreline/contact foam,
// B = freshness/age, A = latest deposit/debug. Bound by name "uOceanFoam" on the
// ocean draw; sampled Linear/Clamp. Its world layout comes from the
// OceanCascadeLayout UBO below.
// Compat: compiled out — the surface's split-sampler budget (one per reflected
// texture) sits exactly at WebGPU's 16-per-stage cap without it. The foam sim
// still runs there; OceanSampleFoamData and OceanSampleFoamLayer read no foam.
#if !defined(GE_COMPAT_PROFILE)
layout(set = 2, binding = 3) uniform sampler2DArray uOceanFoam;
#endif

// The world layout of every camera-snapped cascade the surface samples, one per
// cascade. Each sim snaps its cascade on its own schedule (a bake when its sources
// or snap change, a simulation only on the frames it steps), so each texture is
// sampled with the layout it was written with; a shared layout would place one
// cascade's content where another was snapped. std140 — mirror of
// OceanSampledCascadeLayoutsGPU / OceanCascadeLayoutGPU in OceanTypes.h, indexed by
// OceanSampledCascade. Bound by name "OceanCascadeLayout". Meta.x = LodCount, 0
// when that cascade's sim is not running.
#define GE_OCEAN_CASCADE_FOAM 0
#define GE_OCEAN_CASCADE_SEABED_DEPTH 1
#define GE_OCEAN_CASCADE_FLOW 2
#define GE_OCEAN_CASCADE_DYN_WAVES 3
#define GE_OCEAN_CASCADE_WAVE_MASK 4
#define GE_OCEAN_CASCADE_CLIP 5
#define GE_OCEAN_CASCADE_ALBEDO 6
#define GE_OCEAN_SAMPLED_CASCADES 7
struct OceanCascadeLayoutData
{
    vec4  OriginScale[GE_OCEAN_LOD_CASCADES]; // xy=origin, z=texel, w=scale
    uvec4 Meta;                               // x = LodCount, y = 1 when the coarsest layer is world-anchored
};
layout(std140, set = 2, binding = 4) uniform OceanCascadeLayout
{
    OceanCascadeLayoutData uOceanCascades[GE_OCEAN_SAMPLED_CASCADES];
};

// Compat profile (WebGPU-class): the fragment stage has a 16-samplers-per-stage
// budget shared with the PBR/Forward+ sets, so only the two core cascades above
// (displacement + foam) keep their bindings. Every optional feature texture
// below is compiled out and its sampling helper returns the same neutral value
// it returns at runtime when the feature is unavailable — the sub-sims that
// would fill these textures all decline on narrow-format-less backends anyway.
#if !defined(GE_COMPAT_PROFILE)

// Scene-colour grab (set 2 binding 5): a snapshot of the opaque scene colour
// taken before the ocean draws, so the surface can refract what's behind it.
// Bound by name "uOceanSceneColor"; gated by uRefractionFlags.x (0 = opaque).
// Sampled LinearClamp at the (normal-distorted) screen UV.
layout(set = 2, binding = 5) uniform sampler2D uOceanSceneColor;

// Sea-floor depth cascade (set 2 binding 6): per-LOD camera-snapped distance from
// the calm sea level DOWN to the seabed (meters), baked top-down by
// ocean_seafloor_depth.comp. Placed by its own layout in the OceanCascadeLayout
// block (binding 4). Bound by name "uOceanSeabedDepth";
// gated by uSeabedDepthAvailable. Sampled Linear/Clamp.
layout(set = 2, binding = 6) uniform sampler2DArray uOceanSeabedDepth;

// Procedural caustics texture (set 2 binding 7): a small tileable RGBA8 web of
// refracted-sunlight focusing, generated once at startup. R,G = a tangent-space
// distortion normal (xy, [0,1]-encoded) that bends the caustic lookup UVs; B =
// the caustic intensity (sampled twice at two scrolling scales). Bound by name
// "uOceanCaustics"; gated by uCausticsAvailable (0 = skip). Sampled Linear /
// Repeat / mipmapped (the focal-depth blur selects the mip).
layout(set = 2, binding = 7) uniform sampler2D uOceanCaustics;

// Flow cascade (set 2 binding 8): per-LOD camera-snapped horizontal current
// (RG = world XZ velocity, meters/second), baked from the tagged flow sources by
// ocean_flow_sim.comp. Placed by its own layout in the OceanCascadeLayout block
// (binding 4). Bound by name "uOceanFlow"; gated by
// uFlowAvailable. Drives the foam advection (in the foam compute) and the
// detail-UV scroll below. Sampled Linear/Clamp.
layout(set = 2, binding = 8) uniform sampler2DArray uOceanFlow;

// Dynamic-wave cascade (set 2 binding 9): per-LOD camera-snapped interactive wave
// state (R = height meters, G = velocity), simulated by ocean_dynwaves_sim.comp
// (2D wave equation + impulse injection). Placed by its own layout in the
// OceanCascadeLayout block (binding 4). Bound by name "uOceanDynWaves"; gated by
// uDynamicWavesAvailable. Read in the vertex stage (textureLod) for the
// displacement add and in the fragment stage for the normal fold. Sampled
// Linear/Clamp.
layout(set = 2, binding = 9) uniform sampler2DArray uOceanDynWaves;

// Clip cascade (set 2 binding 10): per-LOD camera-snapped surface clip state
// (R = clip value, 1 = clipped/hole, 0 = solid water), baked from the tagged clip
// sources by ocean_clip_sim.comp over the default clip state. Placed by its own
// layout in the OceanCascadeLayout block (binding 4). Bound by name
// "uOceanClip"; gated by uClipAvailable. The surface discards fragments where the
// sampled value exceeds 0.5. Sampled Linear/Clamp.
layout(set = 2, binding = 10) uniform sampler2DArray uOceanClip;

// Albedo cascade (set 2 binding 11): per-LOD camera-snapped surface paint
// (RGBA = colour + coverage), baked from the tagged albedo sources by
// ocean_albedo_sim.comp. Placed by its own layout in the OceanCascadeLayout block
// (binding 4). Bound by name "uOceanAlbedo"; gated by uAlbedoAvailable.
// The surface blends baseColor toward rgb by a (decals/paint) before lighting +
// under foam. Sampled Linear/Clamp.
layout(set = 2, binding = 11) uniform sampler2DArray uOceanAlbedo;

// User foam-bubble texture (set 2 binding 12): an optional tiling whitecap/bubble
// image the user assigns on the ocean component. When uFoamTextureAvailable, the
// surface samples its red channel for the foam dissolve in place of the analytic
// Worley noise. Bound by name "uOceanFoamBubble". Sampled Linear/Repeat (tiling).
layout(set = 2, binding = 12) uniform sampler2D uOceanFoamBubble;
layout(set = 2, binding = 27) uniform sampler2D uOceanDetailNormal;

// Planar reflection capture (set 2 binding 13): the opaque scene re-rendered from
// the mirror camera (reflected across the sea plane), at half screen resolution.
// rgb = reflected scene colour, a = coverage (0 where nothing drew — sky/background
// — so the surface falls back to the procedural sky dome there). Bound by name
// "uOceanReflection"; gated by uPlanarReflectionAvailable (0 = use the sky dome).
// The mirror camera shares the main projection, so the surface samples it at the
// fragment's screen UV (the reference's _ReflectionTex). Sampled Linear/Clamp.
layout(set = 2, binding = 13) uniform sampler2D uOceanReflection;

// Combined displacement cascade (set 2 binding 14): the 16 tileable FFT cascades
// summed into a camera-snapped viewer cascade (one slice per LOD) by
// ocean_combine.comp, so OceanSampleDisplacement samples ~2 slices instead of 16.
// Has its OWN snapped layout (binding 15) — it can allocate more LODs than the foam
// cascade. Bound by name "uOceanCombinedDisplacement"; gated by
// uCombineWavesAvailable. Sampled Linear/Clamp.
layout(set = 2, binding = 14) uniform sampler2DArray uOceanCombinedDisplacement;

// Combine cascade layout (set 2 binding 15): the snapped per-LOD origin/texel of the
// combined displacement cascade (separate from the foam OceanCascadeLayout because it
// can have a different LOD count). std140 — mirror of OceanCascadeLayoutGPU.
layout(std140, set = 2, binding = 15) uniform OceanCombineCascadeLayout
{
    vec4  uCombineCascadeOriginScale[GE_OCEAN_LOD_CASCADES];
    uvec4 uCombineCascadeMeta; // x = LodCount
};

// Local wave override mask (set 2 binding 16): R = wave weight/amplitude,
// G = horizontal chop scale. Placed by its own layout in the OceanCascadeLayout
// block (binding 4). Bound by name
// "uOceanWaveMask"; gated by uWaveMaskAvailable. Default sample is (1,1,0,0).
layout(set = 2, binding = 16) uniform sampler2DArray uOceanWaveMask;

// Optional local spectrum displacement cascades (set 2 bindings 17-24). These
// streams are used by spectrum-authored bounded water bodies; the local FFT mask
// pages store one blend weight per stream.
layout(set = 2, binding = 17) uniform sampler2DArray uOceanLocalDisplacement0;
layout(set = 2, binding = 18) uniform sampler2DArray uOceanLocalDisplacement1;
layout(set = 2, binding = 19) uniform sampler2DArray uOceanLocalDisplacement2;
layout(set = 2, binding = 20) uniform sampler2DArray uOceanLocalDisplacement3;
layout(set = 2, binding = 21) uniform sampler2DArray uOceanLocalDisplacement4;
layout(set = 2, binding = 22) uniform sampler2DArray uOceanLocalDisplacement5;
layout(set = 2, binding = 23) uniform sampler2DArray uOceanLocalDisplacement6;
layout(set = 2, binding = 24) uniform sampler2DArray uOceanLocalDisplacement7;
layout(set = 2, binding = 25) uniform sampler2DArray uOceanLocalFFTMask0;
layout(set = 2, binding = 26) uniform sampler2DArray uOceanLocalFFTMask1;

#endif // !GE_COMPAT_PROFILE

// Sample the seabed depth (sea-level-to-floor distance, meters) at a world XZ
// point using its own cascade layout. Picks the finest LOD layer whose
// snapped tile contains the point (same selection as OceanSampleFoam). Returns a
// large depth (deep water) when the cascade is inactive so the shallow/shoreline
// terms stay off until a seabed is present.
// Seabed depth from the finest cascade whose texels are at least
// minTexelSize: a reader sampling at a coarse rate (a mesh vertex) must not see
// depth edges its own sampling cannot resolve.
float OceanSampleSeabedDepthAtScale(vec2 worldXZ, float minTexelSize)
{
#if defined(GE_COMPAT_PROFILE)
    return 1.0e6;
#else
    if (uSeabedDepthAvailable == 0u)
        return 1.0e6;
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return 1.0e6;
    float res = float(textureSize(uOceanSeabedDepth, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (layer.z >= minTexelSize && all(greaterThanEqual(uv, vec2(0.04))) &&
            all(lessThanEqual(uv, vec2(0.96))))
            return textureLod(uOceanSeabedDepth, vec3(uv, float(lod)), 0.0).r;
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return textureLod(uOceanSeabedDepth, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0).r;
#endif // GE_COMPAT_PROFILE
}

// Shallow clarity window: the factor the surface scales its depth-fog alpha by.
// Over the first uShallowClarityDistance meters of view-ray path through the
// water it rises from uShallowClarityFloor to 1, so the seabed and its caustics
// stay readable in shallow water whatever the density; past it the fog is
// unscaled. Seen from below the surface (underBelow = 1) there is no window.
float OceanShallowFogScale(float waterDist, float underBelow)
{
    float shallowClarity = smoothstep(0.0, uShallowClarityDistance, waterDist);
    return mix(mix(uShallowClarityFloor, 1.0, shallowClarity), 1.0, underBelow);
}

// The water's ambient light: the procedural sky dome desaturated toward its
// luminance (a real sky's diffuse fill is far less saturated than a flat blue
// dome, and the blue x blue water product would otherwise crush red and green),
// replaced by the environment irradiance as the IBL intensity rises. Shared by
// the surface body and the spray droplets.
vec3 OceanSkyAmbient()
{
    vec3 skyAmbient = mix(uSkyBase.rgb, uSkyTowardsSun.rgb, 0.4);
    const float kOceanAmbientDesat = 0.6;
    float ambientLum = dot(skyAmbient, vec3(0.2126, 0.7152, 0.0722));
    return mix(skyAmbient, vec3(ambientLum), kOceanAmbientDesat);
}

vec3 OceanBlendIblAmbient(vec3 fallbackAmbient, vec3 irradiance, float iblIntensity)
{
    return mix(fallbackAmbient, irradiance * iblIntensity, clamp(iblIntensity, 0.0, 1.0));
}

// Weight of the finer of two cascades at cascade UV `uv`: 1 inside, fading to 0
// toward the tile margin, so a field that each cascade resolves differently
// (a feather floored to two texels of its own cascade, say) changes gradually
// instead of stepping along the tile boundary. Same band as OceanSampleFoamData.
float OceanCascadeFineWeight(vec2 uv)
{
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return smoothstep(0.04, 0.18, edge);
}

float OceanSampleSeabedDepth(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return 1.0e6;
#else
    if (uSeabedDepthAvailable == 0u)
        return 1.0e6;
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return 1.0e6;
    float res = float(textureSize(uOceanSeabedDepth, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
        {
            float fine = textureLod(uOceanSeabedDepth, vec3(uv, float(lod)), 0.0).r;
            if (lod + 1u >= lodCount)
                return fine;
            vec4 next = uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].OriginScale[lod + 1u];
            vec2 nextUV = OceanCascadeUV(worldXZ, next.xy, next.z, res);
            float coarse = textureLod(uOceanSeabedDepth, vec3(nextUV, float(lod + 1u)), 0.0).r;
            return mix(coarse, fine, OceanCascadeFineWeight(uv));
        }
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_SEABED_DEPTH].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return textureLod(uOceanSeabedDepth, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0).r;
#endif // GE_COMPAT_PROFILE
}

// Underwater caustics colour multiplier (clean-room). Fakes refracted-sunlight
// focusing on whatever the water refracts: two tiled, scrolling samples of the
// caustic web (B channel), their UVs distorted by the texture's encoded normal
// (R,G) and offset along the sun's horizontal projection so the web rakes with
// the light. The mip level focuses the web at a chosen seabed distance and blurs
// it away from there (depth-of-field), selected analytically — derivatives would
// dilate/stretch the web on the refracted background. Returns a per-channel
// factor centred on 1.0 (multiply into the refracted scene colour). Returns 1.0
// (no change) when the caustics texture is unavailable.
//   worldXZ   = the surface fragment's world XZ (proxy for the seabed footprint)
//   depthDist = view path length through water to the seabed (meters)
//   sunDir    = unit vector toward the sun
vec3 OceanCaustics(vec2 worldXZ, float depthDist, vec3 sunDir)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec3(1.0);
#else
    if (uCausticsAvailable == 0u)
        return vec3(1.0);

    // Rake the web along the sun's horizontal projection, growing with depth (the
    // /4 fudge tames the directionality so the web doesn't slide wildly with the
    // waves). Guard a near-horizon sun (tiny sunDir.y) so the projection is sane.
    float sunY = max(abs(sunDir.y), 0.15);
    vec2 lightProj = sunDir.xz * depthDist / (4.0 * sunY);
    vec2 basePos = worldXZ + lightProj;

    // Distortion: sample the encoded normal (R,G) at the distortion world scale,
    // decode to [-1,1], and bend the two lookup UVs by it. A user-assigned caustics
    // image has no R,G distortion channel, so skip the distortion for it (its RG
    // would be arbitrary colour and warp the lookup with garbage).
    vec2 distort = vec2(0.0);
    if (uCausticsTextureAvailable == 0u)
    {
        vec2 distUV = worldXZ / max(uCausticsDistortionScale, 1e-3);
        vec2 caustN = texture(uOceanCaustics, distUV).rg * 2.0 - 1.0;
        distort = caustN * uCausticsDistortionStrength;
    }

    // Manual mip: blur away from the focal seabed distance (depth-of-field).
    float mip = abs(depthDist - uCausticsFocalDepth) / max(uCausticsDepthOfField, 1e-3);

    float invScale = 1.0 / max(uCausticsScale, 1e-3);
    float t = uTime;
    // Two scrolling samples at slightly different scales/phases (the reference
    // overlaps two lookups of the same web so the focus shimmers rather than
    // sliding rigidly). Read the B channel — the caustic intensity.
    vec2 uv1 = basePos * invScale + vec2(0.044 * t + 17.16, -0.169 * t) + 1.30 * distort;
    vec2 uv2 = 1.37 * basePos * invScale + vec2(0.248 * t, 0.117 * t) + 1.77 * distort;
    float a = textureLod(uOceanCaustics, uv1, mip).b;
    float b = textureLod(uOceanCaustics, uv2, mip).b;

    // (A*B - Average)*Strength as a multiplier centred on 1.0. The product of the
    // two webs sharpens the focused filaments (both must be bright to show).
    float caustics = (a * b - uCausticsAverage) * uCausticsStrength;
    return vec3(1.0 + caustics);
#endif // GE_COMPAT_PROFILE
}

vec4 OceanSampleFoamLayer(vec2 worldXZ, uint lod, float res)
{
#if defined(GE_COMPAT_PROFILE)
    return vec4(0.0);
#else
    vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_FOAM].OriginScale[lod];
    vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return vec4(0.0);

    vec4 foam = textureLod(uOceanFoam, vec3(uv, float(lod)), 0.0);
    float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    return foam * smoothstep(0.0, 0.04, edge);
#endif // GE_COMPAT_PROFILE
}

// Sample the simulated foam state at a world XZ point. Picks the finest LOD layer
// whose snapped tile contains the point, then blends toward the next coarser LOD
// near tile edges so foam does not pop or seam at cascade boundaries. Returns 0
// when no foam sim is active (LodCount == 0), which keeps the surface on its
// per-pixel Jacobian foam fallback.
vec4 OceanSampleFoamData(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    return vec4(0.0);
#else
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_FOAM].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return vec4(0.0);
    float res = float(textureSize(uOceanFoam, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_FOAM].OriginScale[lod];
        vec2 origin = layer.xy;
        float texel = layer.z;
        vec2 uv = OceanCascadeUV(worldXZ, origin, texel, res);
        // Use this LOD if the point falls inside its tile (with a small margin so
        // the seam between LODs isn't a hard edge); otherwise try the next coarser.
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
        {
            vec4 fine = textureLod(uOceanFoam, vec3(uv, float(lod)), 0.0);
            if (lod + 1u >= lodCount)
                return fine;

            float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
            float fineWeight = smoothstep(0.04, 0.18, edge);
            vec4 coarse = OceanSampleFoamLayer(worldXZ, lod + 1u, res);
            return mix(coarse, fine, fineWeight);
        }
    }
    // Past the coarsest tile, do not clamp to its border texels: any accumulated
    // foam there would smear into long straight lines across the projected ocean.
    return OceanSampleFoamLayer(worldXZ, lodCount - 1u, res);
#endif // GE_COMPAT_PROFILE
}

float OceanSampleFoam(vec2 worldXZ)
{
    vec4 foamData = OceanSampleFoamData(worldXZ);
    return max(foamData.r, foamData.g);
}

// Sample the horizontal flow velocity (world XZ meters/second) at a world XZ
// point using its own cascade layout. Same finest-LOD-that-contains-the-
// point selection as OceanSampleFoam. Returns 0 (no flow) when the flow cascade
// is inactive, so the detail-UV scroll falls back to stationary ripples.
vec2 OceanSampleFlow(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec2(0.0);
#else
    if (uFlowAvailable == 0u)
        return vec2(0.0);
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_FLOW].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return vec2(0.0);
    float res = float(textureSize(uOceanFlow, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_FLOW].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
            return textureLod(uOceanFlow, vec3(uv, float(lod)), 0.0).rg;
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_FLOW].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return textureLod(uOceanFlow, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0).rg;
#endif // GE_COMPAT_PROFILE
}

// Sample the dynamic-wave HEIGHT (meters) at a world XZ point using its own
// cascade layout. Safe in the vertex stage (textureLod, no derivatives).
// Returns 0 when the dynamic-wave cascade is inactive, so the spectrum surface is
// unchanged. The .r channel is height; the .g (velocity) channel is sim-internal.
float OceanSampleDynWaveHeight(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return 0.0;
#else
    if (uDynamicWavesAvailable == 0u)
        return 0.0;
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_DYN_WAVES].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return 0.0;
    float res = float(textureSize(uOceanDynWaves, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_DYN_WAVES].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
            return textureLod(uOceanDynWaves, vec3(uv, float(lod)), 0.0).r;
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_DYN_WAVES].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return textureLod(uOceanDynWaves, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0).r;
#endif // GE_COMPAT_PROFILE
}

// Sample the surface clip state (1 = clipped/hole, 0 = solid) at a world XZ point
// using its own cascade layout. Same finest-LOD-that-contains-the-point
// selection as OceanSampleFoam. Returns the default clip state when the cascade is
// inactive, so the surface stays solid (or clipped) without any clip source.
float OceanSampleClip(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return uDefaultClippingState;
#else
    if (uClipAvailable == 0u)
        return uDefaultClippingState;
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_CLIP].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return uDefaultClippingState;
    float res = float(textureSize(uOceanClip, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_CLIP].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
        {
            float fine = textureLod(uOceanClip, vec3(uv, float(lod)), 0.0).r;
            // The anchored coarsest layer's texels follow the spread of all the
            // authored water, so the outermost camera layer is not blended into it.
            bool nextAnchored = lod + 2u == lodCount && uOceanCascades[GE_OCEAN_CASCADE_CLIP].Meta.y != 0u;
            if (lod + 1u >= lodCount || nextAnchored)
                return fine;
            vec4 next = uOceanCascades[GE_OCEAN_CASCADE_CLIP].OriginScale[lod + 1u];
            vec2 nextUV = OceanCascadeUV(worldXZ, next.xy, next.z, res);
            float coarse = textureLod(uOceanClip, vec3(nextUV, float(lod + 1u)), 0.0).r;
            return mix(coarse, fine, OceanCascadeFineWeight(uv));
        }
    }
    // Beyond the coarsest cascade no source was baked. Clamping to its edge
    // would stretch the edge texels to the horizon, so a river crossing that
    // edge would open a wedge of water out to infinity.
    return uDefaultClippingState;
#endif // GE_COMPAT_PROFILE
}

// Sample the surface albedo paint (rgb = colour, a = coverage) at a world XZ point
// using its own cascade layout. Same finest-LOD-that-contains-the-point
// selection as OceanSampleFoam. Returns transparent (a = 0) when the cascade is
// inactive, so the base water colour is unchanged without any albedo source.
vec4 OceanSampleAlbedo(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec4(0.0);
#else
    if (uAlbedoAvailable == 0u)
        return vec4(0.0);
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_ALBEDO].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return vec4(0.0);
    float res = float(textureSize(uOceanAlbedo, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_ALBEDO].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
            return textureLod(uOceanAlbedo, vec3(uv, float(lod)), 0.0);
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_ALBEDO].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return textureLod(uOceanAlbedo, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0);
#endif // GE_COMPAT_PROFILE
}

vec4 OceanSampleWaveMask(vec2 worldXZ)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec4(1.0, 1.0, 0.0, 0.0);
#else
    if (uWaveMaskAvailable == 0u)
        return vec4(1.0, 1.0, 0.0, 0.0);
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return vec4(1.0, 1.0, 0.0, 0.0);
    float res = float(textureSize(uOceanWaveMask, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
        {
            vec4 m = textureLod(uOceanWaveMask, vec3(uv, float(lod)), 0.0);
            return vec4(max(m.rg, vec2(0.0)), m.ba);
        }
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    vec4 m = textureLod(uOceanWaveMask, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1u)), 0.0);
    return vec4(max(m.rg, vec2(0.0)), m.ba);
#endif // GE_COMPAT_PROFILE
}

float OceanDisplacementBandWeight(float worldSize, float minWorldSize)
{
    if (minWorldSize <= 1e-4)
        return 1.0;
    return smoothstep(minWorldSize, minWorldSize * 2.0, worldSize);
}

float OceanFFTMaxWavelength(uint cascade, uint cascadeCount)
{
    float worldSize = 0.5 * float(1u << cascade);
    // SpectrumInit keeps maxCoord in [4, 8) for every ordinary cascade, so the
    // longest wave in that band is worldSize / 4. The coarsest cascade also keeps
    // the low-frequency tail, whose longest represented wave is the full tile.
    return (cascade + 1u == cascadeCount) ? worldSize : worldSize * 0.25;
}

// Sum the FFT displacement cascades at a world XZ point, optionally dropping
// cascades whose represented wavelengths are below a stable world-space
// footprint. The raw form is still available for exact height queries; render
// shading/geometry can pass a minimum wave size so sub-pixel FFT bands do not
// shimmer when the camera moves.
vec3 OceanSampleDisplacementFilteredAniso(vec2 worldXZ, float minHorizontalWorldSize, float minVerticalWorldSize)
{
    vec3 disp = vec3(0.0);
    vec2 phaseXZ = worldXZ + vec2(uWaveOriginOffsetX, uWaveOriginOffsetZ);
    uint count = min(uFFTCascadeCount, uint(GE_OCEAN_FFT_CASCADES));
    for (uint c = 0u; c < count; ++c)
    {
        float worldSize = 0.5 * float(1u << c);
        float maxWavelength = OceanFFTMaxWavelength(c, count);
        float hBand = OceanDisplacementBandWeight(maxWavelength, minHorizontalWorldSize);
        float vBand = OceanDisplacementBandWeight(maxWavelength, minVerticalWorldSize);
        if (max(hBand, vBand) <= 1e-4)
            continue;
        vec2 uv = phaseXZ / worldSize;
        vec3 d = textureLod(uOceanDisplacement, vec3(uv, float(c)), 0.0).xyz;
        disp += vec3(d.x * hBand, d.y * vBand, d.z * hBand);
    }
    return disp;
}

vec3 OceanSampleDisplacementFiltered(vec2 worldXZ, float minWorldSize)
{
    return OceanSampleDisplacementFilteredAniso(worldXZ, minWorldSize, minWorldSize);
}

vec3 OceanSampleDisplacement(vec2 worldXZ)
{
    return OceanSampleDisplacementFiltered(worldXZ, 0.0);
}

vec3 OceanSampleCombinedDisplacementFilteredAniso(vec2 worldXZ,
                                                  float minHorizontalWorldSize,
                                                  float minVerticalWorldSize)
{
#if defined(GE_COMPAT_PROFILE)
    return OceanSampleDisplacementFilteredAniso(worldXZ, minHorizontalWorldSize,
                                                minVerticalWorldSize);
#else
    uint lodCount = min(uCombineCascadeMeta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (uCombineWavesAvailable == 0u || lodCount == 0u)
        return OceanSampleDisplacementFilteredAniso(worldXZ, minHorizontalWorldSize,
                                                    minVerticalWorldSize);

    float res = float(textureSize(uOceanCombinedDisplacement, 0).x);
    float desiredTexel = 0.5 * min(minHorizontalWorldSize, minVerticalWorldSize);
    uint chosen = 0u;
    bool found = false;
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uCombineCascadeOriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (!all(greaterThanEqual(uv, vec2(0.04))) ||
            !all(lessThanEqual(uv, vec2(0.96))))
            continue;
        chosen = lod;
        found = true;
        if (layer.z >= desiredTexel)
            break;
    }
    if (!found)
        chosen = lodCount - 1u;
    vec4 layer = uCombineCascadeOriginScale[chosen];
    vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
    return textureLod(uOceanCombinedDisplacement,
                      vec3(clamp(uv, 0.0, 1.0), float(chosen)), 0.0).xyz;
#endif // GE_COMPAT_PROFILE
}

vec4 OceanSampleLocalFFTMaskTex(uint page, vec3 uvw)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec4(0.0);
#else
    if (page == 0u)
        return textureLod(uOceanLocalFFTMask0, uvw, 0.0);
    return textureLod(uOceanLocalFFTMask1, uvw, 0.0);
#endif // GE_COMPAT_PROFILE
}

vec4 OceanSampleLocalFFTMask(vec2 worldXZ, uint page)
{
#if defined(GE_COMPAT_PROFILE)
    // Compat: feature texture compiled out; the feature-unavailable value.
    return vec4(0.0);
#else
    if (uLocalFFTAvailable == 0u || uWaveMaskAvailable == 0u)
        return vec4(0.0);
    uint lodCount = min(uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].Meta.x, uint(GE_OCEAN_LOD_CASCADES));
    if (lodCount == 0u)
        return vec4(0.0);
    float res = float(textureSize(uOceanLocalFFTMask0, 0).x);
    for (uint lod = 0u; lod < lodCount; ++lod)
    {
        vec4 layer = uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].OriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
            return clamp(OceanSampleLocalFFTMaskTex(page, vec3(uv, float(lod))), 0.0, 1.0);
    }
    vec4 coarse = uOceanCascades[GE_OCEAN_CASCADE_WAVE_MASK].OriginScale[lodCount - 1u];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return clamp(OceanSampleLocalFFTMaskTex(page,
                                            vec3(clamp(uv, 0.0, 1.0),
                                                 float(lodCount - 1u))),
                 0.0, 1.0);
#endif // GE_COMPAT_PROFILE
}

vec3 OceanSampleLocalFFTDisplacementFilteredAniso(vec2 worldXZ,
                                                  uint stream,
                                                  float minHorizontalWorldSize,
                                                  float minVerticalWorldSize)
{
#if defined(GE_COMPAT_PROFILE)
    return vec3(0.0);
#else
    vec3 disp = vec3(0.0);
    vec2 phaseXZ = worldXZ + vec2(uWaveOriginOffsetX, uWaveOriginOffsetZ);
    uint count = min(uFFTCascadeCount, uint(GE_OCEAN_FFT_CASCADES));
    for (uint c = 0u; c < count; ++c)
    {
        float worldSize = 0.5 * float(1u << c);
        float maxWavelength = OceanFFTMaxWavelength(c, count);
        float hBand = OceanDisplacementBandWeight(maxWavelength, minHorizontalWorldSize);
        float vBand = OceanDisplacementBandWeight(maxWavelength, minVerticalWorldSize);
        if (max(hBand, vBand) <= 1e-4)
            continue;
        vec2 uv = phaseXZ / worldSize;
        vec3 local = vec3(0.0);
        if (stream == 0u)
            local = textureLod(uOceanLocalDisplacement0, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 1u)
            local = textureLod(uOceanLocalDisplacement1, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 2u)
            local = textureLod(uOceanLocalDisplacement2, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 3u)
            local = textureLod(uOceanLocalDisplacement3, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 4u)
            local = textureLod(uOceanLocalDisplacement4, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 5u)
            local = textureLod(uOceanLocalDisplacement5, vec3(uv, float(c)), 0.0).xyz;
        else if (stream == 6u)
            local = textureLod(uOceanLocalDisplacement6, vec3(uv, float(c)), 0.0).xyz;
        else
            local = textureLod(uOceanLocalDisplacement7, vec3(uv, float(c)), 0.0).xyz;
        disp += vec3(local.x * hBand, local.y * vBand, local.z * hBand);
    }
    return disp;
#endif // GE_COMPAT_PROFILE
}

vec3 OceanSampleLocalFFTDisplacementFiltered(vec2 worldXZ, uint stream, float minWorldSize)
{
    return OceanSampleLocalFFTDisplacementFilteredAniso(worldXZ, stream, minWorldSize, minWorldSize);
}

vec3 OceanSampleLocalFFTDisplacement(vec2 worldXZ, uint stream)
{
    return OceanSampleLocalFFTDisplacementFiltered(worldXZ, stream, 0.0);
}

// FFT displacement with the Shape FFT authoring controls applied: overall Weight,
// shallow-water attenuation (flatten toward the shoreline by the seabed depth
// cascade), and the max horizontal/vertical displacement clamps. Used by BOTH the
// vertex stage (geometry) and the fragment macro normal so they stay consistent.
// Inert at the defaults (Weight 1, huge clamps, no seabed) — identical to the raw
// displacement.
vec3 OceanShapeDisplacementFilteredAniso(vec2 worldXZ, float minHorizontalWorldSize, float minVerticalWorldSize)
{
    vec4 waveMask = OceanSampleWaveMask(worldXZ);
    vec3 d = OceanSampleCombinedDisplacementFilteredAniso(
        worldXZ, minHorizontalWorldSize, minVerticalWorldSize);
    if (uLocalFFTAvailable != 0u)
    {
        vec4 localWeights0 = OceanSampleLocalFFTMask(worldXZ, 0u);
        if ((uLocalFFTAvailable & 1u) != 0u && localWeights0.x > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 0u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights0.x);
        if ((uLocalFFTAvailable & 2u) != 0u && localWeights0.y > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 1u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights0.y);
        if ((uLocalFFTAvailable & 4u) != 0u && localWeights0.z > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 2u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights0.z);
        if ((uLocalFFTAvailable & 8u) != 0u && localWeights0.w > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 3u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights0.w);

        vec4 localWeights1 = OceanSampleLocalFFTMask(worldXZ, 1u);
        if ((uLocalFFTAvailable & 16u) != 0u && localWeights1.x > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 4u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights1.x);
        if ((uLocalFFTAvailable & 32u) != 0u && localWeights1.y > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 5u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights1.y);
        if ((uLocalFFTAvailable & 64u) != 0u && localWeights1.z > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 6u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights1.z);
        if ((uLocalFFTAvailable & 128u) != 0u && localWeights1.w > 1e-4)
            d = mix(d, OceanSampleLocalFFTDisplacementFilteredAniso(
                           worldXZ, 7u, minHorizontalWorldSize, minVerticalWorldSize),
                    localWeights1.w);
    }
    float atten = uWeight;
    if (uRespectShallowAttenuation > 0.0)
    {
        // Seabed depth: 0 at the shoreline, large in deep water (huge when no
        // seabed cascade is present, so deep water keeps full waves). Fade the
        // waves out as the water thins, blended by RespectShallowAttenuation.
        // Band-limited to the displacement's own resolution: a depth edge
        // sharper than the mesh spacing would fold into faceted walls.
        float seabed = OceanSampleSeabedDepthAtScale(worldXZ, 0.5 * minVerticalWorldSize);
        float shallow = clamp(seabed / max(uSubSurfaceDepthMax, 1.0), 0.0, 1.0);
        atten *= mix(1.0, shallow, uRespectShallowAttenuation);
    }
    d *= atten * waveMask.x;
    d.xz *= waveMask.y;
    d.y += waveMask.z;
    d.xz = clamp(d.xz, vec2(-uMaxHorizontalDisplacement), vec2(uMaxHorizontalDisplacement));
    d.y  = clamp(d.y, -uMaxVerticalDisplacement, uMaxVerticalDisplacement);
    return d;
}

vec3 OceanShapeDisplacementFiltered(vec2 worldXZ, float minWorldSize)
{
    return OceanShapeDisplacementFilteredAniso(worldXZ, minWorldSize, minWorldSize);
}

vec3 OceanShapeDisplacement(vec2 worldXZ)
{
    return OceanShapeDisplacementFiltered(worldXZ, 0.0);
}

// Analytic Gerstner surface used by explicitly-authored Gerstner oceans and as
// the device/query fallback when FFT data is unavailable. It consumes the same
// wave-mask and local-height cascade as the FFT path so bounded water overrides
// remain coherent across both representations.
void EvaluateGerstner(vec2 worldXZ, out vec3 outPos, out vec3 outNormal, out float outFoam)
{
    vec2 phaseXZ = worldXZ + vec2(uWaveOriginOffsetX, uWaveOriginOffsetZ);
    vec3 pos = vec3(worldXZ.x, uSeaLevel, worldXZ.y);
    vec3 tangent = vec3(1.0, 0.0, 0.0);
    vec3 binormal = vec3(0.0, 0.0, 1.0);
    vec4 waveMask = OceanSampleWaveMask(worldXZ);

    float jacobian = 1.0;
    uint count = min(uGerstnerWaveCount, uint(GE_OCEAN_MAX_GERSTNER));
    for (uint i = 0u; i < count; ++i)
    {
        GerstnerWaveGPU w = uWaves[i];
        vec2 dir = vec2(w.DirectionX, w.DirectionZ);
        float dirLen = max(length(dir), 1e-4);
        dir /= dirLen;

        float k = 6.2831853 / max(w.Wavelength, 1e-3);
        float c = sqrt(GE_OCEAN_GRAVITY / k) * w.Speed;
        float wf = k * c;
        float q = w.Steepness * uChoppyScale * waveMask.y;
        float amp = w.Amplitude * waveMask.x;

        float phase = k * dot(dir, phaseXZ) + uTime * wf;
        float s = sin(phase);
        float co = cos(phase);
        float wa = wf * amp;

        pos.x += q * amp * dir.x * co;
        pos.z += q * amp * dir.y * co;
        pos.y += amp * s;
        tangent += vec3(-q * dir.x * dir.x * wa * s,
                        dir.x * wa * co,
                        -q * dir.x * dir.y * wa * s);
        binormal += vec3(-q * dir.x * dir.y * wa * s,
                         dir.y * wa * co,
                         -q * dir.y * dir.y * wa * s);
        jacobian -= q * k * amp * s;
    }

    pos.y += waveMask.z;
    if (abs(waveMask.z) > 1e-5)
    {
        const float e = 0.5;
        float hL = OceanSampleWaveMask(worldXZ - vec2(e, 0.0)).z;
        float hR = OceanSampleWaveMask(worldXZ + vec2(e, 0.0)).z;
        float hD = OceanSampleWaveMask(worldXZ - vec2(0.0, e)).z;
        float hU = OceanSampleWaveMask(worldXZ + vec2(0.0, e)).z;
        tangent.y += (hR - hL) / (2.0 * e);
        binormal.y += (hU - hD) / (2.0 * e);
    }

    outPos = pos;
    outNormal = normalize(cross(binormal, tangent));
    outFoam = clamp(1.0 - jacobian, 0.0, 1.0);
}

// Cheap per-pixel high-frequency normal detail (analytic micro-ripples). Adds
// the fine surface sparkle that a full FFT spectrum would provide, without any
// texture. Returns a tangent-space-ish perturbation normal (Y up).
vec3 OceanDetailNormal(vec2 worldXZ, float time)
{
    worldXZ += vec2(uWaveOriginOffsetX, uWaveOriginOffsetZ);
    const vec2 d1 = vec2(0.80, 0.60);
    const vec2 d2 = vec2(-0.50, 0.86);
    const vec2 d3 = vec2(0.20, -0.98);
    const float k1 = 1.30, k2 = 2.10, k3 = 3.70;
    const float a1 = 0.060, a2 = 0.040, a3 = 0.022;

    float p1 = dot(d1, worldXZ) * k1 + time * 1.7;
    float p2 = dot(d2, worldXZ) * k2 + time * 2.3;
    float p3 = dot(d3, worldXZ) * k3 + time * 3.1;

    // slope = d/dXZ of sum(a*sin(phase)) = sum(a*k*cos(phase)*dir)
    vec2 slope = a1 * k1 * cos(p1) * d1
               + a2 * k2 * cos(p2) * d2
               + a3 * k3 * cos(p3) * d3;
    return normalize(vec3(-slope.x, 1.0, -slope.y));
}

#endif // GE_OCEAN_COMMON_GLSL
