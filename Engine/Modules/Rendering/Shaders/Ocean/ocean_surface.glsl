// Ocean surface shader. Implements EvaluateSurface() for the ocean material
// adapter pipeline. Lighting: view-weighted diffuse scattering, sun-driven
// subsurface forward scatter, IOR-Schlick Fresnel over a procedural sky dome,
// and persistent-foam whitecaps. Refraction/depth/caustics layer on later.
//
// All shading constants now come from OceanParamsBuffer (set 2, binding 1) so
// they are authorable per-ocean in the inspector.

#include "Ocean/ocean_common.glsl"
#include "Ocean/ocean_surface_material.glsl"
// ge_sceneDepth (set 0 b17) + the reverse-Z eye-depth reconstruction + the screen-UV->texel clamp
// now live in the shared Includes/screen_space.glsl (also backs the StandardPBR transmission lobe),
// so the ocean no longer carries byte-identical copies. ge_sceneDepth is the opaque (resolved)
// depth the world pass binds to every material; the ocean reads it to find how far the view ray
// travels through water to the seabed. (Requires ge_invProj/ge_screenSize from view_params.glsl,
// which the forward adapter includes before this surface shader.)
#include "Includes/screen_space.glsl"

// Requires: Includes/surface_io.glsl (included by the adapter)

float OceanDepthFogAutoRange(vec3 density)
{
    float maxDensity = max(max(density.r, density.g), max(density.b, 1.0e-3));
    return 8.0 / maxDensity;
}

float OceanDepthFogShapedDistance(float waterDist, vec3 density)
{
    float startDist = max(uDepthFogFalloff.x, 0.0);
    float clearDist = max(waterDist - startDist, 0.0);
    float power = max(uDepthFogFalloff.z, 0.01);
    float mode = uDepthFogFalloff.w;

    if (mode < 0.5)
        return pow(clearDist, power);

    float endDist = uDepthFogFalloff.y > startDist
        ? uDepthFogFalloff.y
        : startDist + OceanDepthFogAutoRange(density);
    float range = max(endDist - startDist, 1.0e-3);
    float t = clamp(clearDist / range, 0.0, 1.0);
    if (mode > 1.5)
        t = t * t * (3.0 - 2.0 * t);
    t = pow(t, power);
    return t * range;
}

vec3 OceanDepthFogAlpha(vec3 density, float waterDist)
{
    float shapedDist = OceanDepthFogShapedDistance(waterDist, density);
    return vec3(1.0) - exp(-density * shapedDist);
}

// Keep refraction's color footprint on the same depth surface. Ordinary bilinear
// filtering pulls bright sky across an object edge even when the depth lookup
// selects that object, leaving a pale contour after shallow-water extinction.
// Used when the MSAA grab kept only the samples on the resolved depth surface.
vec3 OceanRefractionColor(vec2 uv, float referenceDepth)
{
#if defined(GE_COMPAT_PROFILE)
    // The compatibility profile has no scene-color grab.
    return vec3(0.0);
#else
    ivec2 extent = textureSize(uOceanSceneColor, 0);
    vec2 at = uv * vec2(extent) - 0.5;
    ivec2 base = ivec2(floor(at));
    vec2 blend = fract(at);
    float tolerance = max(referenceDepth * 0.005, 1e-6);
    vec3 color = vec3(0.0);
    float total = 0.0;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
        {
            ivec2 p = clamp(base + ivec2(x, y), ivec2(0), extent - 1);
            vec2 sampleUV = (vec2(p) + 0.5) / vec2(extent);
            float depth = texelFetch(ge_sceneDepth, GE_ScreenTexel(sampleUV), 0).r;
            float weight = (x == 0 ? 1.0 - blend.x : blend.x)
                         * (y == 0 ? 1.0 - blend.y : blend.y)
                         * step(abs(depth - referenceDepth), tolerance);
            color += texelFetch(uOceanSceneColor, p, 0).rgb * weight;
            total += weight;
        }
    return color / max(total, 1e-6);
#endif
}

vec3 OceanApplyIblReflection(vec3 fallbackSky, vec3 reflectionDir, float roughness)
{
#ifdef GE_IBL_ENABLED
    float iblBlend = clamp(Env.iblIntensity, 0.0, 1.0);
    float lod = clamp(roughness, 0.0, 1.0) * Env.prefilterMaxMip;
    // Deliberately sample the lossless environment directly. The ocean owns its
    // horizon/self-occlusion response and must not inherit terrestrial IBL ground
    // darkening from GE_SampleEnvironmentPrefilter.
    vec3 iblSky = textureLod(ge_prefilterCube, normalize(reflectionDir), lod).rgb * Env.iblIntensity;
    return mix(fallbackSky, iblSky, iblBlend);
#else
    return fallbackSky;
#endif
}

vec3 OceanApplyIblAmbient(vec3 fallbackAmbient, vec3 normalDir)
{
#ifdef GE_IBL_ENABLED
    // Same opt-out for the radiometric water body. Foam/paint use the explicit
    // SurfaceOutput participation flag set below.
    return OceanBlendIblAmbient(fallbackAmbient, texture(ge_irradianceCube, normalize(normalDir)).rgb,
                                Env.iblIntensity);
#else
    return fallbackAmbient;
#endif
}

#if !defined(GE_COMPAT_PROFILE)
layout(set = 2, binding = 28) uniform sampler2DArray uOceanShadows;
layout(std430, set = 2, binding = 29) readonly buffer OceanShadowInfo
{
    vec4 oceanShadowTiles[7];
    uvec4 oceanShadowMeta;
    vec4 oceanShadowChannels;
};
#endif
bool OceanAccumulatedShadowAvailable()
{
#if defined(GE_COMPAT_PROFILE)
    return false;
#else
    return oceanShadowChannels.z >= 0.5;
#endif
}
vec2 OceanAccumulatedShadow(vec2 xz, float fallback)
{
#if !defined(GE_COMPAT_PROFILE)
    if (!OceanAccumulatedShadowAvailable())
        return vec2(fallback);
    for (uint lod = 0u; lod < min(oceanShadowMeta.x, 7u); ++lod)
    {
        vec4 tile = oceanShadowTiles[lod];
        vec2 uv = (xz - tile.xy) / (tile.z * float(textureSize(uOceanShadows, 0).x));
        if (all(greaterThan(uv, vec2(0.01))) && all(lessThan(uv, vec2(0.99))))
        {
            vec2 lit = texture(uOceanShadows, vec3(uv, lod)).rg;
            if (lod + 1u < oceanShadowMeta.x)
            {
                vec4 coarseTile = oceanShadowTiles[lod + 1u];
                vec2 coarseUV = (xz - coarseTile.xy) / (coarseTile.z * float(textureSize(uOceanShadows, 0).x));
                vec2 coarse = texture(uOceanShadows, vec3(coarseUV, lod + 1u)).rg;
                float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                lit = mix(coarse, lit, smoothstep(0.01, 0.1, edge));
            }
            return clamp(1.0 - (1.0 - lit) * oceanShadowChannels.xy, 0.0, 1.0);
        }
    }
#endif
    return vec2(fallback);
}

vec2 OceanHash2(vec2 p)
{
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return fract(sin(p) * 43758.5453);
}

// Cellular (Worley) noise — distance to the nearest feature point. Sea foam is
// bubbly/cellular, so this is the texture-free fallback for the tiling
// foam texture. Returns ~0 at bubble centres,
// rising toward cell edges; callers invert for rounded bubbles.
float OceanWorley(vec2 p)
{
    vec2 ip = floor(p);
    vec2 fp = fract(p);
    float d = 1.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 g = vec2(float(x), float(y));
            vec2 o = OceanHash2(ip + g);
            vec2 r = g + o - fp;
            d = min(d, dot(r, r));
        }
    }
    return sqrt(d);
}

// Two-scale bubbly foam, drifting with time: large foam clumps modulated by fine
// bubbles — the reference samples its foam texture at two scales the same way.
float OceanFoamTexture(vec2 p, float time)
{
    float macro = 1.0 - OceanWorley(p * 0.6 + vec2(time * 0.03, time * 0.02));
    float micro = 1.0 - OceanWorley(p * 2.3 - vec2(time * 0.05, time * 0.04));
    return clamp(macro * 0.65 + micro * 0.5, 0.0, 1.0);
}

// Foam bubble value at a pre-scaled UV: the user's foam texture (red channel) when
// one is assigned, otherwise the analytic two-scale Worley above. Centralizes the
// override so every foam read (dissolve, tint, bubble underlayer) honors it.
float OceanFoamBubble(vec2 p, float time)
{
#if !defined(GE_COMPAT_PROFILE)
    if (oceanHasMaterial)
    {
        if (oceanMaterial.foamTexture != 0xffffffffu)
            return texture(GE_BTEX(oceanMaterial.foamTexture, GE_TS_REPEAT), p).r;
        return OceanFoamTexture(p, time);
    }
    if (uFoamTextureAvailable != 0u)
        return texture(uOceanFoamBubble, p).r;
#endif
    return OceanFoamTexture(p, time);
}

// True when this fragment's foam comes from an assigned texture: the water-body
// material's, else the ocean's. The textured foam path (flow-advected pattern,
// lace tone, relief, submerged bubbles) is tuned for a texture; without one the
// procedural foam keeps its own path whatever the detail normal does.
bool OceanFoamTextureAssigned()
{
#if !defined(GE_COMPAT_PROFILE)
    if (oceanHasMaterial)
        return oceanMaterial.foamTexture != 0xffffffffu;
    return uFoamTextureAvailable != 0u;
#else
    return false;
#endif
}

// Adapted from Crest OceanFoam.hlsl / OceanNormalMapping.hlsl (MIT; see
// Engine/Modules/Ocean/NOTICE). Two overlapping phases keep the flow distortion
// bounded. A phase wraps only while its contribution is zero.
vec3 OceanFlowPhases(float time)
{
    float phase = mod(time, 2.0);
    return vec3(phase, mod(time + 1.0, 2.0), 1.0 - abs(phase - 1.0));
}

float OceanFlowFoam(vec2 worldXZ, vec2 flow, float scale, vec2 offset)
{
    vec3 phase = OceanFlowPhases(uTime);
    vec2 base = worldXZ / max(scale, 0.01) + offset;
    if (uFlowAvailable == 0u)
        return OceanFoamBubble(base, uTime);
    vec2 velocity = flow / max(scale, 0.01);
    float a = OceanFoamBubble(base - velocity * phase.x, uTime);
    float b = OceanFoamBubble(base - velocity * phase.y, uTime);
    return mix(b, a, phase.z);
}

vec2 OceanNormalLayer(vec2 worldXZ, float tileSize)
{
    vec2 uv = worldXZ / tileSize;
    // Different pixels can choose adjacent scale bands. Differentiate the world
    // coordinate before dividing by the local tile size, so a band boundary does
    // not create a spurious mip spike from the discontinuous scale selection.
    vec2 dx = dFdx(worldXZ) / tileSize, dy = dFdy(worldXZ) / tileSize;
    vec2 a = OceanMaterialNormalSample(uv + uTime * vec2(0.025, 0.018), dx, dy) * 2.0 - 1.0;
    // Rotate the coordinates, gradients and decoded slopes of the second sample.
    vec2 b = OceanMaterialNormalSample(vec2(-uv.y, uv.x) + uTime * vec2(-0.017, 0.023), vec2(-dx.y, dx.x),
                                       vec2(-dy.y, dy.x)) *
                 2.0 -
             1.0;
    return 0.5 * (a + vec2(b.y, -b.x));
}

vec2 OceanLayeredNormals(vec2 worldXZ, float footprint)
{
    // Fade between adjacent powers-of-two scales, keeping distant detail
    // resolvable without a visible ring at a cascade boundary.
    float level = max(log2(max(footprint * 32.0, 1.0)), 0.0);
    float tile = 8.0 * exp2(floor(level));
    return mix(OceanNormalLayer(worldXZ, tile),
               OceanNormalLayer(worldXZ, tile * 2.0), fract(level));
}

vec3 OceanFlowNormal(vec2 worldXZ, vec2 flow, float footprint)
{
    vec3 phase = OceanFlowPhases(uTime);
    vec2 a = worldXZ - flow * phase.x;
    vec2 b = worldXZ - flow * phase.y;
    vec2 origin = vec2(uWaveOriginOffsetX, uWaveOriginOffsetZ) * uNormalsScale;
    vec2 slope = mix(OceanLayeredNormals(b + origin, footprint),
                     OceanLayeredNormals(a + origin, footprint), phase.z);
    return normalize(vec3(slope.x, 1.0, slope.y));
}

vec3 OceanTintPositiveHighlights(vec3 factor, vec3 tint)
{
    vec3 darkening = min(factor, vec3(1.0));
    vec3 highlight = max(factor - vec3(1.0), vec3(0.0));
    float highlightPeak = max(max(highlight.r, highlight.g), highlight.b);
    if (highlightPeak <= 1.0e-5)
        return factor;

    vec3 highlightTint = max(tint, vec3(0.0));
    float tintPeak = max(max(highlightTint.r, highlightTint.g), max(highlightTint.b, 1.0e-3));
    highlightTint /= tintPeak;
    return darkening + highlight * highlightTint;
}

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    OceanResolveSurfaceMaterial(sIn.uv0);
    // The detailed surface (filtered normal-map ripples, foam relief, submerged
    // bubbles, filtered glint) shades only where a detail normal texture is
    // assigned. Without one, every term below is the analytic surface's.
    bool detailedSurface = OceanMaterialNormalAvailable() != 0u;
    SurfaceOutput o = DefaultSurfaceOutput();
    o.iblGroundDarkening = 0.0;
    vec3 V = normalize(sIn.viewDirWS);
    float viewDist = max(length(sIn.positionWS - ge_cameraPosWS.xyz), 1.0);
    float pixelAngle = 2.0 / max(ge_screenSize.y, 1.0);
    float grazing = 1.0 / max(abs(V.y), 0.08);
    float surfaceFootprint = viewDist * pixelAngle * grazing;

    // Clip surface (Phase 10): cut holes in the water (harbors, boat interiors).
    // The clip cascade stores a per-texel clip state baked from the tagged clip
    // sources; where it exceeds 0.5 the fragment is discarded so nothing renders
    // (no colour, no depth) — leaving whatever is behind the water visible. Done
    // first so a clipped fragment costs no further shading. Inert (returns the
    // default state) when the cascade is inactive, so the surface stays solid.
    if (OceanSampleClip(sIn.uv0) > 0.5)
        discard;

    // macroN = the wave normal that drives foam, depth tint, screen-space lookups,
    // shadows, and depth lookups. Filtered detail adds to the shading normal for
    // Fresnel, sky reflections and PBR lighting without perturbing contact depth
    // samples or shadow bias.
    vec3 macroN;
    float fftFoam = 0.0;
    vec2 foamSampleXZ = sIn.uv0;
    if (uFFTCascadeCount > 0u)
    {
        // Per-pixel macro normal from the FFT HEIGHT gradient, so shading stays
        // smooth no matter how coarse the camera-following grid is (the grid only
        // carries the silhouette; the normal carries the detail). Height is
        // single-valued, so its slope normal is always well-defined — unlike a
        // full-displacement cross product, which degenerates where the choppy
        // horizontal displacement folds the surface (those folds become foam).
        vec2 worldXZ = sIn.uv0;
        // Avoid using dFdx/dFdy(worldXZ) for FFT band selection here. On the
        // camera-following ocean mesh those derivatives are constant per triangle,
        // so the selected bands change in visible diagonal steps. Use a
        // continuous view-space footprint instead, similar in spirit to Crest's
        // smooth LOD alpha: distance and grazing angle decide which wavelengths a
        // pixel can resolve, independent of the triangle edge layout.
        float minWaveSize = max(0.5, surfaceFootprint * 2.0);
        float e = clamp(minWaveSize * 0.5, 0.5, 4.0);
        vec3 dL = OceanShapeDisplacementFiltered(worldXZ - vec2(e, 0.0), minWaveSize);
        vec3 dR = OceanShapeDisplacementFiltered(worldXZ + vec2(e, 0.0), minWaveSize);
        vec3 dD = OceanShapeDisplacementFiltered(worldXZ - vec2(0.0, e), minWaveSize);
        vec3 dU = OceanShapeDisplacementFiltered(worldXZ + vec2(0.0, e), minWaveSize);
        macroN = normalize(vec3(dL.y - dR.y, 2.0 * e, dD.y - dU.y));

        // Foam from the horizontal-displacement Jacobian (the reference). Where the
        // choppy displacement folds the surface (J < ~0), waves are pinching /
        // breaking — that is where whitecaps form. Threshold kept conservative so
        // foam accents breaking crests rather than blanketing the surface.
        float inv2e = 1.0 / (2.0 * e);
        float dDxdx = (dR.x - dL.x) * inv2e;
        float dDzdz = (dU.z - dD.z) * inv2e;
        float dDxdz = (dU.x - dD.x) * inv2e;
        float dDzdx = (dR.z - dL.z) * inv2e;
        float jacobian = (1.0 + dDxdx) * (1.0 + dDzdz) - dDxdz * dDzdx;
        fftFoam = smoothstep(uWaveFoamCoverage, -0.15, jacobian);
        fftFoam *= fftFoam;
    }
    else
    {
        macroN = normalize(sIn.normalWS);
    }

    // Dynamic (interactive) wave normal fold: tilt the macro normal by the slope
    // of the simulated ripple height so the interactive ripples shade correctly
    // (the vertex stage already added their displacement). Central difference of
    // the height field; inert when the sim is inactive (returns 0 height).
    if (uDynamicWavesAvailable != 0u)
    {
        vec2 wXZ = sIn.uv0;
        float de = 0.5;
        float dynScale = OceanSampleWaveMask(wXZ).x;
        float hL = OceanSampleDynWaveHeight(wXZ - vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hR = OceanSampleDynWaveHeight(wXZ + vec2(de, 0.0)) * uDynWavesAmplitude * dynScale;
        float hD = OceanSampleDynWaveHeight(wXZ - vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        float hU = OceanSampleDynWaveHeight(wXZ + vec2(0.0, de)) * uDynWavesAmplitude * dynScale;
        vec3 dynN = normalize(vec3(hL - hR, 2.0 * de, hD - hU));
        macroN = normalize(macroN + vec3(dynN.x, 0.0, dynN.z));
    }

    // Flow: scroll the high-frequency detail-normal UVs by the local current so
    // the micro-ripples drift downstream rather than sitting in place. The flow
    // cascade stores world-space velocity (m/s); FlowDetailScale tunes the visual
    // strength. Zero when the flow cascade is inactive (ripples stay stationary).
    vec2 flowVel = OceanSampleFlow(sIn.uv0);

    // Band-limit the high-frequency detail normal at grazing angles. The waves are
    // world-locked; this keeps shimmer down without keying the pattern to camera
    // movement. The detailed surface advects its texture detail in two
    // overlapping phases, so the pattern never stretches without bound; the
    // analytic detail scrolls with the current.
    float detailFade = smoothstep(0.06, 0.35, abs(dot(macroN, V)));
    float detailFootprint = surfaceFootprint * uNormalsScale;
    vec3 detail;
    if (detailedSurface)
    {
        detail = OceanFlowNormal(sIn.uv0 * uNormalsScale, flowVel * uNormalsScale * uFlowDetailScale,
                                 detailFootprint);
    }
    else
    {
        vec2 detailUV = sIn.uv0 - flowVel * uTime * uFlowDetailScale;
        detailFade *= 1.0 - smoothstep(0.35, 1.25, detailFootprint);
        detail = OceanDetailNormal(detailUV * uNormalsScale, uTime);
    }
    // Keep geometric normals for contact and shadow bias; the filtered shading
    // normal adds ripple detail to reflections and surface lighting.
    vec3 NmacroTop = normalize(macroN);
    vec3 Ntop = normalize(NmacroTop + vec3(detail.x, 0.0, detail.z) * detailFade * 0.5 * uNormalsStrength);

    // Back faces (SV_IsFrontFace equivalent). When the camera is underwater the
    // ocean material renders two-sided, so the underside rasterizes with
    // gl_FrontFacing == false and a normal still pointing up out of the water. The
    // forward adapter already flips the OUTPUT normal (o.normalWS) for back faces.
    // Here we only flip local shading normals so underside-only terms evaluate
    // against the wetted underside. underBelow also gates the subsurface glow.
    // Above water (uUnderwater == 0) the material is one-sided — never triggers.
    float underBelow = (uUnderwater != 0u && !gl_FrontFacing) ? 1.0 : 0.0;
    vec3 N = mix(Ntop, -Ntop, underBelow);
    vec3 Nmacro = mix(NmacroTop, -NmacroTop, underBelow);

    vec3 sunDir = normalize(uSunDirection.xyz); // toward the sun
    vec3 sunCol = uSunColor.rgb;

    // The detailed surface reflects and weights Fresnel with its filtered
    // shading normal; the analytic surface keeps the wave normal so unfiltered
    // ripples cannot alias the reflection.
    vec3 Nreflect = detailedSurface ? N : Nmacro;
    float ndv = max(dot(Nreflect, V), 0.0);

    float waveHeight = sIn.positionWS.y - uSeaLevel;
    float crestMask = clamp(0.4 + waveHeight * 0.7, 0.0, 1.0); // raised crests

    // --- Diffuse scattering (clean-room) ---
    // The lit water diffuses between a grazing tint (looking across the surface)
    // and an up-look tint (looking down into it), keyed on |view.y|. In shadow it
    // tints toward DiffuseShadow; with no shadow term yet the surface reads fully
    // lit. A shallow-water multiplier shapes it toward the shoreline tint by the
    // seabed depth (from the Phase 5 depth cascade; deep water elsewhere).
    float viewUp = abs(V.y);
    vec3 scatter = mix(uDiffuseGrazing.rgb, uDiffuse.rgb, viewUp);
    // Directional cascaded-shadow term: terrain/objects between the sun and the
    // water darken the diffuse scatter (and the sun glitter below), matching the
    // reference which samples the sun's shadow map onto the surface. Guarded by
    // HAS_SHADOWS (the ocean material requests the Shadows keyword); the global
    // ge_ReceiveShadows honors the per-instance receiveShadows toggle. Use the
    // macro normal for shadow bias so fine ripple detail cannot jitter shadows.
    float shadow = 1.0;
#ifdef HAS_SHADOWS
    shadow = GE_SampleShadow(sIn.positionWS, Nmacro, sIn.linearDepth, sunDir, gl_FragCoord.xy);
#endif
    // Accumulated water shadows darken the shallow tint too; the engine shadow
    // alone darkens the deep scatter before the shallow mix.
    bool accumulatedShadows = OceanAccumulatedShadowAvailable();
    if (!accumulatedShadows)
        scatter = mix(uDiffuseShadow.rgb, scatter, shadow);
    vec2 accumulatedShadow = OceanAccumulatedShadow(sIn.uv0, shadow);
    shadow = accumulatedShadow.y;
    // Seabed depth from the top-down depth cascade (Phase 5): sea-level-to-floor
    // distance. The shallow factor fades the deep-water scatter toward the
    // shoreline tint as the water thins; inactive cascade returns a huge depth so
    // shallow == 0 (deep water) and nothing changes.
    float seabedDepth = OceanSampleSeabedDepth(sIn.uv0);
    float shallow = pow(1.0 - clamp(seabedDepth / max(uSubSurfaceDepthMax, 1e-3), 0.0, 1.0),
                        uSubSurfaceDepthPower);
    scatter = mix(scatter, uSubSurfaceShallowCol.rgb, shallow);
    if (accumulatedShadows)
        scatter = mix(uDiffuseShadow.rgb, scatter, shadow);

    // --- Subsurface forward scatter (Crest ScatterColour) ---
    // Light transmits through thin raised crests toward the camera: a base term
    // everywhere plus a forward-scatter lobe toward the sun, gated by view grazing
    // (1 - v*v, v = |view.y|) so it concentrates near the horizon and fades looking
    // straight down. Crest's note on this term: "add light when surface faces
    // viewer. Use geometry normal - don't need high freqs" — its sss weight is
    // sampled from the LOD-filtered (mipped) wave data, so it is inherently smooth.
    // Our old fold keyed the glow off the raw per-pixel wave height (crestMask) AND
    // the detail shading normal (1 - N.V); both carry the FFT's fine ripples, so
    // from above the scatter aliased into cyan speckle (worse at high Strength).
    // Use a world-space crest weight and the view-only (1 - v*v) gate (matching Crest).
    float v = viewUp;
    // V points from the surface to the camera, while sunlight travels from the
    // sun along -sunDir. Thin-water translucency is strongest when the view ray
    // looks into that incoming light path, so use the incoming-sun direction.
    float sunLobe = pow(max(dot(-sunDir, V), 0.0), uSubSurfaceSunFallOff);
    float fold = crestMask;
    // Use the sun's HUE, not its raw intensity, so a bright light doesn't wash out.
    vec3 sunTint = sunCol / max(max(sunCol.r, sunCol.g), max(sunCol.b, 1e-3));
    vec3 sss = (uSubSurfaceBase + uSubSurfaceSun * sunLobe)
             * uSubSurfaceColour.rgb * mix(vec3(1.0), sunTint, 0.5)
             * uSubsurfaceStrength * (1.0 - v * v) * fold;

    // --- Procedural sky dome reflection (no IBL) ---
    // Reflected sky steered relative to the sun: blend SkyBase toward AwayFromSun
    // by the away-from-sun weight, then toward TowardsSun by towards^Directionality.
    vec3 R = reflect(-V, Nreflect);
    float sunUp = max(dot(R, sunDir), 0.0);
    float away = clamp(1.0 - sunUp, 0.0, 1.0);
    float towards = pow(sunUp, max(uSkyDirectionality, 1e-3));
    vec3 sky = mix(mix(uSkyBase.rgb, uSkyAwayFromSun.rgb, away), uSkyTowardsSun.rgb, towards);
    sky = OceanApplyIblReflection(sky, R, max(uRoughness, 0.02));

    // Planar reflection: when a mirror-camera capture was rendered + bound this
    // frame, sample it at the fragment's screen UV (the mirror shares the main
    // projection, so the capture aligns to screen space — the reference's
    // _ReflectionTex). Distort the lookup by the surface normal's horizontal
    // component so ripples bend the reflection. The capture holds re-rendered OPAQUE
    // geometry only; its alpha is coverage, so blend it OVER the procedural sky dome
    // (which fills the sky/background the inactive reflection view never drew).
    // Inert (gate 0) until the planar capture pass runs, leaving the sky dome as the
    // sole reflection. When viewed from below, the same planar mirror gives the
    // underside something real to reflect instead of only the procedural sky dome.
    vec3 planarMirrorScene = sky;
#if !defined(GE_COMPAT_PROFILE)
    if (uPlanarReflectionAvailable != 0u)
    {
        // The capture is rendered with a flipped-Y viewport (the winding fix), so
        // its image is vertically mirrored vs a normal render — flip V to read it
        // upright at this fragment's screen position. Distort by the normal's
        // horizontal component so ripples bend the reflection.
        vec2 reflBase = vec2(sIn.screenUV.x, 1.0 - sIn.screenUV.y);
        vec2 reflUV = clamp(reflBase + Nmacro.xz * 0.05, vec2(0.0), vec2(1.0));
        vec4 planar = texture(uOceanReflection, reflUV);
        // Strength <= 1 blends sky -> capture as before. Strength > 1 over-drives
        // the blend so the planar capture can be pushed brighter than the sky
        // (intensity up to 3); clamp >= 0 so the extrapolation never goes negative
        // where the capture is darker than the procedural sky.
        float planarWeight = clamp(planar.a * uPlanarReflectionStrength, 0.0, max(uPlanarReflectionStrength, 1.0));
        planarMirrorScene = max(mix(sky, planar.rgb, planarWeight), vec3(0.0));
        sky = planarMirrorScene;
    }
#endif // GE_COMPAT_PROFILE

    // IOR-driven Schlick Fresnel: base reflectance from the air/water boundary.
    float iorRatio = (uIorAir - uIorWater) / (uIorAir + uIorWater);
    float R0 = iorRatio * iorRatio;
    float fresnel = R0 + (1.0 - R0) * pow(1.0 - ndv, uFresnelPower);

    // Lit water: the deep-water body lit by the sky ambient, the view-weighted
    // diffuse scatter added on top, then forward-scatter subsurface. col*ambient
    // + sss (the reference shape). With no IBL the deep colour carries the living
    // water body so troughs read teal rather than black, and the near-black
    // diffuse-scatter colours add their directional tint over it.
    vec3 skyAmbient = OceanApplyIblAmbient(OceanSkyAmbient(), NmacroTop);

    // The water body is RADIOMETRIC (radiance out): scatter/deep colours lit by
    // the physical sky ambient, plus subsurface. It is emitted via o.emissive at
    // full weight below and must NEVER ride o.baseColor — the PBR pass would
    // multiply it by the physical sun's diffuse (~2^9 scene-linear for a
    // 100k-lux sun) and blow the whole surface out to white. Only true
    // REFLECTANCES (foam, paint) go through baseColor.
    vec3 col = (uDeepColor.rgb + scatter) * skyAmbient;
    vec3 waterBody = col + sss;

    // --- Albedo cascade (Phase 11): decals / paint on the surface ---
    // Paint is a real REFLECTANCE, so it exits via o.baseColor (sun/IBL lit)
    // rather than tinting the radiometric body; here the paint's coverage only
    // OCCLUDES the water radiance underneath it.
    vec4 albedoPaint = OceanSampleAlbedo(sIn.uv0);
    waterBody *= (1.0 - albedoPaint.a);

    // --- Screen-space refraction + depth-fog transparency ---
    // Sample the grabbed opaque scene under the surface, offset by the wave
    // normal so ripples distort what's below, then fade it toward the lit water
    // body by how far the view ray travels through water (scene depth minus this
    // fragment's depth). Thin water shows the seabed; deep water hides it.
    // Per-channel DepthFogDensity gives the blue-survives-longest water falloff.
    // Falls back to the opaque body when the grab declined (flag == 0).
    vec3 body = waterBody;
    float refractionClarity = 0.0;
#if !defined(GE_COMPAT_PROFILE)
    if (uRefractionFlags.x != 0u)
    {
        vec2 screenUV = sIn.screenUV;
        // Distort the grab UV by the surface normal's horizontal component.
        vec2 grabUV = clamp(screenUV + Nmacro.xz * uDepthFogRefraction.a * 0.05,
                            vec2(0.0), vec2(1.0));

        // Reconstruct the underwater path length (reverse-Z). Reject grab texels
        // whose seabed is actually IN FRONT of the surface (a foreground object
        // between camera and water): there, fall back to the undistorted UV so
        // the object doesn't bleed across the water edge.
        float sceneRaw = texelFetch(ge_sceneDepth, GE_ScreenTexel(grabUV), 0).r;
        float fragEye  = GE_EyeDepthFromRaw(gl_FragCoord.z, screenUV);
        float sceneEye = GE_EyeDepthFromRaw(sceneRaw, grabUV);
        if (sceneEye < fragEye)
        {
            sceneRaw = texelFetch(ge_sceneDepth, GE_ScreenTexel(screenUV), 0).r;
            sceneEye = GE_EyeDepthFromRaw(sceneRaw, screenUV);
            grabUV = screenUV;
        }
        float waterDist = max(sceneEye - fragEye, 0.0);

        // Per-channel extinction: alpha rises with depth so the water body takes
        // over; sky (rawDepth ~0 → huge sceneEye) reads as fully deep water.
        vec3 fogAlpha = OceanDepthFogAlpha(uDepthFogRefraction.rgb, waterDist);
        fogAlpha *= OceanShallowFogScale(waterDist, underBelow);
        vec3 sceneColour = uRefractionFlags.y != 0u ? OceanRefractionColor(grabUV, sceneRaw)
                                                    : texture(uOceanSceneColor, grabUV).rgb;
        // Underwater caustics: refracted-sunlight focusing modulates whatever the
        // water refracts, before the depth fog blends it toward the water body.
        // Driven by the path length to the seabed (waterDist) for the focal blur.
        vec3 throughCaustics = OceanCaustics(sIn.uv0, waterDist, sunDir);
        throughCaustics = OceanTintPositiveHighlights(
            throughCaustics, mix(sunTint, uSubSurfaceShallowCol.rgb, 0.45));
        sceneColour *= throughCaustics;
        body = mix(sceneColour, waterBody, clamp(fogAlpha, 0.0, 1.0));
        float causticVisibility = (1.0 - max(max(fogAlpha.r, fogAlpha.g), fogAlpha.b))
                                * (1.0 - underBelow);
        refractionClarity = causticVisibility;
        // Leave caustics single-pass: re-multiplying after fog turns bright shallow
        // highlights on pale seabeds into flat gray-white plates.
    }
#endif // GE_COMPAT_PROFILE

    // Underwater, the water/air boundary becomes a mirror at grazing angles. Let the
    // planar capture carry reflected underwater objects on the surface underside,
    // while keeping near-normal views mostly transparent to the scene grab/body.
    float reflectionWeight = clamp(fresnel * uSpecular * uReflectionStrength, 0.0, 1.0);
    if (underBelow > 0.0 && uPlanarReflectionAvailable != 0u)
    {
        float undersideMirror = smoothstep(0.72, 0.18, ndv) * uReflectionStrength
                               * clamp(uPlanarReflectionStrength, 0.0, 1.0);
        reflectionWeight = max(reflectionWeight, clamp(undersideMirror, 0.0, 1.0) * underBelow);
    }
    float shallowReflectionTarget =
        mix(1.0, 0.28, clamp(uShallowRefractionReflectionSuppression, 0.0, 1.0));
    reflectionWeight *= mix(1.0, shallowReflectionTarget, clamp(refractionClarity, 0.0, 1.0));

    // Sky reflection blended in by Fresnel (Specular caps it so the water reads
    // through on near-normal faces; ReflectionStrength dials the whole reflection).
    body = mix(body, sky, reflectionWeight);

    // --- Foam (reference whitecaps) ---
    // Coverage comes from the persistent foam simulation when it is active (foam
    // accumulates on breaking crests + trails behind waves over seconds); when the
    // sim is unavailable it falls back to the per-pixel FFT displacement Jacobian.
    // The very steepest macro faces always contribute. A bubbly value-noise texture
    // cuts into the coverage so foam reads as whitecaps/bubbles, not a flat white wash.
    vec4 foamData = OceanSampleFoamData(foamSampleXZ);
    float waveFoam = (uOceanCascades[GE_OCEAN_CASCADE_FOAM].Meta.x > 0u) ? clamp(foamData.r, 0.0, 1.0) : fftFoam;
    float shoreFoam = (uOceanCascades[GE_OCEAN_CASCADE_FOAM].Meta.x > 0u) ? clamp(foamData.g, 0.0, 1.0) : 0.0;
    float foamFresh = (uOceanCascades[GE_OCEAN_CASCADE_FOAM].Meta.x > 0u) ? clamp(foamData.b, 0.0, 1.0) : 1.0;
    float foamDepositDebug = (uOceanCascades[GE_OCEAN_CASCADE_FOAM].Meta.x > 0u) ? clamp(foamData.a, 0.0, 1.0) : fftFoam;
    float breaking = waveFoam;
    float crest = clamp(1.0 - macroN.y, 0.0, 1.0);
    // Shoreline foam: a band of whitecaps where the water thins to the seabed.
    // The foam sim already bakes this into the cascade when active; adding it here
    // too keeps the per-pixel fallback (no foam sim) foaming the shoreline.
    // Gated to zero when the depth cascade is inactive (huge depth).
    float shoreline = max(shoreFoam, smoothstep(uShorelineFoamMaxDepth, 0.0, seabedDepth) * uShorelineFoamStrength);
    // Intersection foam: a Wicked-style waterline from scene-depth proximity.
    // Absolute eye-depth difference keeps the contact stable at silhouettes and
    // slightly interpenetrating geometry; layered foam noise breaks the line into
    // bubbles/streaks instead of drawing a hard contour.
    float intersection = 0.0;
    if (uIntersectionFoamStrength > 0.0)
    {
        float ifFragEye  = GE_EyeDepthFromRaw(gl_FragCoord.z, sIn.screenUV);
        float ifSceneRaw = texelFetch(ge_sceneDepth,
                                      GE_ScreenTexel(gl_FragCoord.xy * ge_screenSize.zw), 0).r;
        float ifSceneEye = GE_EyeDepthFromRaw(ifSceneRaw, sIn.screenUV);
        float ifWaterDist = abs(ifSceneEye - ifFragEye);
        float ifDepth = max(uIntersectionFoamDepth, 0.01);
        float line = exp(-ifWaterDist * (6.0 / ifDepth));
        float wash = exp(-ifWaterDist * (2.0 / ifDepth));

        vec2 contactUV = foamSampleXZ / max(uFoamScale, 0.01);
        float contactClumps = OceanFoamBubble(contactUV * 0.85 + vec2(uTime * 0.07, -uTime * 0.035), uTime);
        float contactBubbles = OceanFoamBubble(contactUV * 2.6 - vec2(uTime * 0.04, uTime * 0.055), uTime);
        float breakup = smoothstep(0.18, 0.82, contactClumps * 0.65 + contactBubbles * 0.45);

        intersection = clamp((line + wash * breakup * 0.75) * uIntersectionFoamStrength, 0.0, 1.0);
    }
    float steepCrest = smoothstep(0.74, 0.98, crest) * smoothstep(0.0, 0.6, waveHeight);
    float coverage = max(max(max(breaking, shoreline), intersection), steepCrest) * uFoamAmount;
    if (uFoamDebugMode != 0u)
    {
        float dbg = 0.0;
        vec3 dbgColor = vec3(0.0);
        if (uFoamDebugMode == 1u)
            dbg = clamp(max(waveFoam, shoreFoam), 0.0, 1.0);
        else if (uFoamDebugMode == 2u)
            dbg = waveFoam;
        else if (uFoamDebugMode == 3u)
            dbg = clamp(max(shoreline, intersection), 0.0, 1.0);
        else if (uFoamDebugMode == 4u)
            dbg = foamFresh;
        else if (uFoamDebugMode == 6u)
            dbgColor = vec3(clamp(seabedDepth / 50.0, 0.0, 1.0));
        else if (uFoamDebugMode == 7u)
            dbgColor = mix(vec3(0.02, 0.08, 0.22), vec3(0.2, 0.95, 0.75), shallow);
        else if (uFoamDebugMode == 8u)
        {
            bool invalid = (uSeabedDepthAvailable == 0u) || (seabedDepth > 59000.0);
            dbgColor = invalid ? vec3(1.0, 0.08, 0.0) : vec3(0.05, 0.85, 0.22);
        }
        else
            dbg = foamDepositDebug;
        if (uFoamDebugMode < 6u || uFoamDebugMode > 8u)
            dbgColor = vec3(dbg);

        o.baseColor = dbgColor;
        o.normalWS = NmacroTop;
        o.metallic = 0.0;
        o.roughness = 0.6;
        o.opacity = 1.0;
        o.ao = 1.0;
        o.emissive = dbgColor;
        return o;
    }
    // Crest WhiteFoamTexture: sample the foam texture at the same rest/world XZ
    // where the foam sim deposited it, not at the horizontally displaced crest,
    // otherwise whitecaps slide off the wave as choppiness increases.
    // scaled by 1.25/FoamScale and scrolled by time, then a black-point fade —
    // foam = smoothstep(1 - coverage, 1 - coverage + Feather, foamTex) — so whitecap
    // edges dissolve into bubbles instead of a hard line. FoamScale sets the world
    // tile; FoamFeather the dissolve width.
    float blackPoint = clamp(1.0 - coverage, 0.0, 1.0);
    float foamTex;
    float foam;
    vec3 foamCol;
    vec3 foamNormal = NmacroTop;
    bool texturedFoam = detailedSurface && OceanFoamTextureAssigned();
    if (texturedFoam)
    {
        float foamTile = max(uFoamScale, 0.01) / 1.25;
        vec2 foamDrift = vec2(uTime * 0.1);
        foamTex = OceanFlowFoam(foamSampleXZ, flowVel, foamTile, foamDrift);
        float feather = mix(max(uFoamFeather, 0.001) * 1.35,
                            max(uFoamFeather, 0.001) * 0.65, foamFresh);
        // Antialias the dissolve so thin bubble rims survive subpixel footprints.
        feather = max(feather, fwidth(foamTex));
        foam = smoothstep(blackPoint, blackPoint + feather, foamTex);
        foam *= clamp(coverage * 8.0, 0.0, 1.0);
        // Brightness follows how far the pattern clears the threshold, so a
        // whitecap is densest in its core and thins to grey lace at its rim.
        float foamDensity = smoothstep(blackPoint, 1.0, foamTex);
        foamCol = uFoamColor.rgb * mix(0.82, 1.0, foamFresh) * mix(0.72, 1.0, foamDensity);

        // Recover the pattern's world-space gradient from screen derivatives. This
        // shares the coverage lookup rather than repeating the costly procedural
        // fallback for offset samples. Solve the two-dimensional gradient system;
        // nearly edge-on/degenerate projections receive no relief.
        vec2 px = dFdx(foamSampleXZ), py = dFdy(foamSampleXZ);
        vec2 df = vec2(dFdx(foamTex), dFdy(foamTex));
        float determinant = px.x * py.y - px.y * py.x;
        if (abs(determinant) > 1.0e-10)
        {
            vec2 slope = vec2(df.x * py.y - df.y * px.y,
                              df.y * px.x - df.x * py.x) / determinant;
            slope = clamp(slope * foamTile * 0.05, vec2(-1.0), vec2(1.0));
            // Relief fades with distance and with thin coverage: sparse foam is a
            // film of bubbles, not a crumpled sheet.
            float reliefFade = (1.0 - smoothstep(0.02, 0.15, surfaceFootprint / foamTile)) *
                               smoothstep(0.0, 0.6, coverage);
            foamNormal = normalize(NmacroTop - vec3(slope.x, 0.0, slope.y) *
                                   uFoamNormalStrength * reliefFade);
        }
    }
    else
    {
        vec2 foamUV = 1.25 * foamSampleXZ / max(uFoamScale, 0.01) + vec2(uTime * 0.1);
        foamTex = OceanFoamBubble(foamUV, uTime);
        float feather = mix(max(uFoamFeather, 0.001) * 1.35,
                            max(uFoamFeather, 0.001) * 0.65,
                            foamFresh);
        foam = smoothstep(blackPoint, blackPoint + feather, foamTex);
        foamCol = uFoamColor.rgb * (0.70 + 0.35 * foamTex);
        foamCol *= mix(0.82, 1.12, foamFresh);
    }
    // baseColor is strictly REFLECTANCE: foam + paint albedo, over a near-black
    // water diffuse (real water upwelling albedo is ~0.02-0.06 and is already
    // carried by the radiometric scatter terms). The radiometric body goes out
    // via o.emissive below — mixing radiance into baseColor let the physical
    // sun's diffuse multiply already-lit sky radiance and whited out the sea.
    vec3 paintAlbedo = albedoPaint.rgb * albedoPaint.a;
    vec3 baseColor = mix(paintAlbedo, foamCol, foam);

    // Foam-bubble underlayer (Crest BubbleFoamTexture / _FoamBubbleColor): a faint
    // aerated tint below the whitecaps. It scrolls along wind and distorts by the
    // world-locked surface normal so it does not slide when the camera moves.
    const vec2 foamWindDir = vec2(0.866, 0.5);
    float albedoCoverage;
    if (texturedFoam)
    {
        float bubbleTile = max(uFoamScale, 0.01) / 0.74;
        // Bounded parallax suggests bubbles below the surface without a grazing-angle
        // singularity. Use the geometric normal to avoid sparkle in this broad layer.
        vec2 parallax = -uFoamBubbleParallax * V.xz / max(abs(dot(Nmacro, V)), 0.25);
        vec2 bubbleOffset = 0.5 * uTime * foamWindDir / bubbleTile +
                            0.125 * Nmacro.xz + parallax;
        float bubbleLayer = OceanFlowFoam(foamSampleXZ, flowVel, bubbleTile, bubbleOffset) *
                           clamp(coverage, 0.0, 1.0) * (1.0 - foam) * uFoamBubbleCoverage;
        // Submerged bubbles receive water's ambient illumination and absorption tint;
        // they do not acquire the dry foam's direct surface specular lobe.
        vec3 bubbleTint = mix(uFoamColor.rgb, uSubSurfaceShallowCol.rgb, 0.35);
        body = mix(body, bubbleTint * OceanApplyIblAmbient(vec3(0.12), NmacroTop), bubbleLayer);
        albedoCoverage = clamp(max(foam, albedoPaint.a), 0.0, 1.0);
    }
    else
    {
        vec2 bubbleUV = (foamSampleXZ + 0.5 * uTime * foamWindDir) * (0.74 / max(uFoamScale, 0.01)) +
                        0.125 * Nmacro.xz;
        float bubbleLayer = OceanFoamBubble(bubbleUV, uTime) * clamp(coverage, 0.0, 1.0) * (1.0 - foam);
        baseColor = mix(baseColor, uFoamColor.rgb * 0.55, bubbleLayer * 0.35);
        // How much reflectance now covers this fragment: the radiometric body is
        // occluded underneath it.
        albedoCoverage = clamp(max(foam, max(albedoPaint.a, bubbleLayer * 0.35)), 0.0, 1.0);
    }
    // --- Directional-light glitter: crisp specular spike toward the sun ---
    // FallOff sets the lobe tightness (sharper on calm/glassy faces, broader on
    // foam); Boost scales the spike. Roughness widens the lobe like a microfacet
    // surface would.
    // Filter the sharp lobe by the normal variation within this pixel. Preserve
    // its approximate integrated energy while widening it, rather than turning
    // unresolved ripple normals into bright single-pixel flashes.
    // The analytic surface keeps an unfiltered lobe; its world-locked
    // normal/detail path handles stability.
    // Sun glitter is direct sunlight off the surface, so shadowed water shows none.
    vec3 glitter;
    if (detailedSurface)
    {
        vec3 normalDx = dFdx(N), normalDy = dFdy(N);
        float normalVariance = 0.5 * (dot(normalDx, normalDx) + dot(normalDy, normalDy));
        float glintFall = max(mix(uDirectionalLightFallOff, 60.0,
                                 clamp(foam + uRoughness, 0.0, 1.0)), 1.0);
        float filteredFall = max(glintFall / (1.0 + 2.0 * glintFall * normalVariance), 1.0);
        float glint = pow(max(dot(R, sunDir), 0.0), filteredFall) *
                      (filteredFall + 1.0) / (glintFall + 1.0);
        glitter = sunCol * uDirectionalLightColor.rgb * glint * uDirectionalLightBoost * (1.0 - foam) *
                  accumulatedShadow.x;
    }
    else
    {
        float glintAA = 1.0;
        float glintFall = mix(uDirectionalLightFallOff, 60.0, foam + uRoughness);
        glintFall = mix(40.0, glintFall, glintAA);
        float glint = pow(max(dot(R, sunDir), 0.0), max(glintFall, 1.0));
        glitter = sunCol * uDirectionalLightColor.rgb * glint * uDirectionalLightBoost
                * (1.0 - foam) * glintAA * accumulatedShadow.x;
    }

    o.baseColor = baseColor;
    // Output the topside normal; the forward adapter flips it for back faces
    // (two-sided underwater) so PBR lighting faces the viewer. N above is the
    // local shading normal already flipped for the underside math.
    // With a detail normal the lit normal carries the ripples; foam relief bends
    // it only where a foam texture drives the relief (foamNormal stays NmacroTop
    // otherwise).
    o.normalWS  = detailedSurface ? normalize(mix(Ntop, foamNormal, foam)) : NmacroTop;
    o.metallic  = 0.0;
    o.roughness = clamp(mix(uRoughness, texturedFoam ? uFoamRoughness : 0.5, foam), 0.0, 1.0);
    o.opacity   = 1.0;
    o.ao        = 1.0;

    // The complete radiometric water body (ambient-lit scatter + subsurface +
    // refraction + Fresnel sky/planar reflection) plus the sun glitter, occluded
    // where foam/paint reflectance covers the surface. The glitter stays raw
    // sun radiance: it is a mirror spike, and real sun glints ARE display-white.
    o.emissive = (body + glitter) * (1.0 - albedoCoverage);

    // Subsurface from below: looking up at the underside, sunlight filters down
    // through the water and lights the surface from the air side, so the wetted
    // ceiling glows with the shallow/subsurface tint, brightest toward the sun's
    // refracted disc (Snell's window). Added only on back faces while submerged.
    // Sun HUE only (matching the sss term above): raw physical intensity here
    // would white out the underside.
    if (underBelow > 0.0)
    {
        float window = pow(max(dot(N, sunDir), 0.0), 3.0); // bright near the refracted sun
        vec3 underGlow = uSubSurfaceShallowCol.rgb * (0.35 + 0.65 * window)
                       + sunTint * window * 0.5;
        o.emissive += underGlow * uSubsurfaceStrength * (1.0 - albedoCoverage);
    }
    o.subsurfaceColor = uSubSurfaceShallowCol.rgb;
    o.thickness = clamp(0.3 + waveHeight * 0.5, 0.0, 1.0);

    return o;
}
