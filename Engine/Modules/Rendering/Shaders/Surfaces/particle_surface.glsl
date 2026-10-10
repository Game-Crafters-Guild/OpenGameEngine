// Compact particles, sprite sheets, and optional six-way volume lighting.
// The renderer enables GE_USER_PARTICLE_BUFFER for its compact upload arena.
// @texture albedoMap srgb
// @texture positiveAxesMap linear
// @texture negativeAxesMap linear
// @texture emissionMap srgb
// @texture wingMaskMap srgb
// @property float hybridAlphaBlend "Hybrid Alpha Blend" default=0 range=0,1 group=Texture
// @property float hybridColorMultiplier "Hybrid Color Multiplier" default=1 range=0,10 group=Texture
// @property float alphaFromGrayscale "Alpha From Grayscale" default=0 range=0,1 group=Texture
// @property float trailTextureTile "Tile Trail Texture" default=0 range=0,1 group=Texture
// @property float wingFlutter "Wing Flutter" default=0 range=0,1 group=Wings
// @property color wingColor1 "Wing Color 1" default=1,1,1,1 group=Wings
// @property color wingColor2 "Wing Color 2" default=1,1,1,1 group=Wings
// @property color baseColor default=1,1,1,1
// @property color emissionColor default=1,1,1,1 group=Emission
// @property float emissionIntensity "Emission Intensity" default=0 range=0,1000000 group=Emission
// @property float gpuFogSimpleNoiseScale default=20 hidden
// @property float gpuFogSimplexNoiseScale default=4 hidden
// @property float gpuFogVoronoiScale default=5 hidden
// @property float gpuFogCombinedNoiseRemap default=0 hidden
// @property float gpuFogSimpleNoiseAmount default=0.25 hidden
// @property float gpuFogSimplexNoiseAmount default=0.25 hidden
// @property float gpuFogVoronoiNoiseAmount default=0.5 hidden
// @property float gpuFogRadialMaskPower default=1.35 hidden
// @property float gpuFogSimpleNoiseRemap default=0 hidden
// @property float gpuFogSimplexNoiseRemap default=0 hidden
// @property float gpuFogVoronoiNoiseRemap default=0 hidden
// @property float gpuFogEdgeSoftness default=0.22 hidden
// @property float gpuFogSurfaceDepthFade default=0.66 hidden
// @property float gpuFogShapeDistortion default=0.28 hidden
// @property float gpuFogWispyNoiseAmount default=0.35 hidden
// @property float gpuFogDetailNoiseAmount default=0.25 hidden
// @property float gpuFogSimpleAnimationX default=0 hidden
// @property float gpuFogSimpleAnimationY default=0 hidden
// @property float gpuFogSimpleAnimationZ default=0 hidden
// @property float gpuFogSimpleAnimationW default=0 hidden
// @property float gpuFogSimplexAnimationX default=0 hidden
// @property float gpuFogSimplexAnimationY default=0 hidden
// @property float gpuFogSimplexAnimationZ default=0.02 hidden
// @property float gpuFogSimplexAnimationW default=0 hidden
// @property float gpuFogVoronoiAnimationX default=0 hidden
// @property float gpuFogVoronoiAnimationY default=0 hidden
// @property float gpuFogVoronoiAnimationZ default=0 hidden
// @property float gpuFogVoronoiAnimationW default=0 hidden
// @property float gpuFogCameraDepthFadeRange default=1 hidden
// @property float gpuFogCameraDepthFadeOffset default=0 hidden
// @property float sheetColumns "Sheet Columns" default=1 range=1,256 group=Animation
// @property float sheetRows "Sheet Rows" default=1 range=1,256 group=Animation
// @property float sheetFrames "Frame Count" default=1 range=1,65536 group=Animation
// @property float sheetBlend "Blend Frames" default=1 range=0,1 group=Animation
// @property float sheetLoop "Loop" default=1 range=0,1 group=Animation
// @property float sixWayStrength "Six-way Contrast" default=1 range=0,4 group=Lighting
// The draw's own constant, set by the particle renderer: 1 when the draw blends by straight alpha
// (source alpha, one minus source alpha), where a fragment of opacity 0 draws nothing.
// @property float straightAlphaBlend default=0 hidden
// The draw's own constant, set by the particle renderer (NearFadeColorExponent, ParticleMaterials.cpp):
// the power of the near fade this draw's colour and emission take beyond what its alpha gives them.
// @property float nearFadeColorExponent default=0 hidden

#include "Includes/particle_coverage.glsl"
#include "Includes/particle_six_way.glsl"

vec2 ParticleSheetDimensions(GE_MaterialTexture map)
{
#if defined(GE_COMPAT_PROFILE)
    switch (map.Slot)
    {
    case 0u: return vec2(textureSize(sampler2D(ge_MaterialTextures0, ge_MaterialSamplers0), 0));
    case 1u: return vec2(textureSize(sampler2D(ge_MaterialTextures1, ge_MaterialSamplers1), 0));
    case 2u: return vec2(textureSize(sampler2D(ge_MaterialTextures2, ge_MaterialSamplers2), 0));
    case 3u: return vec2(textureSize(sampler2D(ge_MaterialTextures3, ge_MaterialSamplers3), 0));
    case 4u: return vec2(textureSize(sampler2D(ge_MaterialTextures4, ge_MaterialSamplers4), 0));
    case 5u: return vec2(textureSize(sampler2D(ge_MaterialTextures5, ge_MaterialSamplers5), 0));
    case 6u: return vec2(textureSize(sampler2D(ge_MaterialTextures6, ge_MaterialSamplers6), 0));
    default: return vec2(textureSize(sampler2D(ge_MaterialTextures7, ge_MaterialSamplers7), 0));
    }
#else
    return vec2(textureSize(sampler2D(ge_BindlessTextures[nonuniformEXT(map.TexIdx)],
                                     ge_BindlessSamplers[nonuniformEXT(map.SamplerIdx)]), 0));
#endif
}

vec2 ParticleSheetUV(vec2 uv, float frame, GE_MaterialTexture map)
{
    vec2 grid = max(vec2(Props.sheetColumns, Props.sheetRows), vec2(1.0));
    if (grid == vec2(1.0) && Props.trailTextureTile > 0.5) return fract(uv);
    vec2 cell = vec2(mod(frame, grid.x), floor(frame / grid.x));
    vec2 dimensions = ParticleSheetDimensions(map);
    vec2 inset = min(vec2(0.49), grid * 0.5 / max(dimensions, vec2(1.0)));
    return (cell + clamp(uv, inset, vec2(1.0) - inset)) / grid;
}

// The screen-space footprint of a sheet sample: the particle UV's change per pixel along x and y,
// taken before any discard (EvaluateSurface), divided down to one cell of the sheet.
struct ParticleSheetFootprint
{
    vec2 uvPerPixelX;
    vec2 uvPerPixelY;
};

// Props.sheetBlend is the draw's own constant, 1 only for a sheet with frames to blend, so a draw that
// does not blend takes one fetch on every fragment alike. Every fetch takes its level from the
// footprint, never from the derivatives of its own coordinate (EvaluateSurface states why).
vec4 ParticleSheetSample(GE_MaterialTexture map, vec2 uv, float frame, float next, float blend,
                         ParticleSheetFootprint footprint)
{
    vec2 grid = max(vec2(Props.sheetColumns, Props.sheetRows), vec2(1.0));
    vec2 perPixelX = footprint.uvPerPixelX / grid;
    vec2 perPixelY = footprint.uvPerPixelY / grid;
    vec4 a = textureGrad(map, ParticleSheetUV(uv, frame, map), perPixelX, perPixelY);
    if (Props.sheetBlend < 0.5) return a;
    return mix(a, textureGrad(map, ParticleSheetUV(uv, next, map), perPixelX, perPixelY), blend);
}

#include "Includes/gpu_fog_shape.glsl"

SurfaceOutput EvaluateSurface(SurfaceInput sIn)
{
    SurfaceOutput o = DefaultSurfaceOutput();

    // Asset thumbnails and ordinary mesh passes do not bind particle instance data.
    vec4 animation = vec4(0.0);
    vec4 lifetimeColor = vec4(1.0);
#ifdef GE_USER_PARTICLE_BUFFER
    animation = sIn.particleAnimation;
    lifetimeColor = sIn.custom0;
#endif
    // Every screen-space gradient this surface needs is taken here, before any discard, so a discard
    // never feeds a derivative: a discarded pixel stops contributing to its 2x2 quad, and a gradient
    // taken after it is undefined for the pixels that keep shading. The case that needs it is a
    // trail's zero end: a trail's alpha interpolates between a segment's ends (particle_vertex.glsl),
    // a fading trail's end alpha is exactly 0, and the helper pixels of a quad straddling that end
    // carry alpha extrapolated below 0 and are discarded while their neighbours still sample.
    ParticleSheetFootprint footprint = ParticleSheetFootprint(dFdx(sIn.uv0), dFdy(sIn.uv0));
    // A draw that blends by straight alpha discards a fragment as soon as its opacity is known to be 0
    // (GE_ParticleDrawsNothing). Only unlit particles discard: after a lit surface the adapter's
    // lighting samples shadow maps and the environment and its specular anti-aliasing takes
    // derivatives, none of which a discard may precede.
    bool discardTransparent = Props.straightAlphaBlend > 0.5;
#if !defined(GE_USER_PARTICLE_LIT)
    if (discardTransparent && GE_ParticleDrawsNothing(Props.baseColor.a * sIn.vertexColor.a * lifetimeColor.a))
        discard;
#endif
    float timeSeconds = animation.x;
    float particleStableRandom = fract(animation.w);
    vec2 uv = sIn.uv0;

    vec4 p0 = vec4(Props.gpuFogSimpleNoiseScale, Props.gpuFogSimplexNoiseScale, Props.gpuFogVoronoiScale, Props.gpuFogCombinedNoiseRemap);
    vec4 p1 = vec4(Props.gpuFogSimpleNoiseAmount, Props.gpuFogSimplexNoiseAmount, Props.gpuFogVoronoiNoiseAmount, Props.gpuFogRadialMaskPower);
    vec4 p2 = vec4(Props.gpuFogSimpleNoiseRemap, Props.gpuFogSimplexNoiseRemap, Props.gpuFogVoronoiNoiseRemap, Props.gpuFogEdgeSoftness);
    vec4 p4 = vec4(Props.gpuFogSurfaceDepthFade, Props.gpuFogShapeDistortion, Props.gpuFogWispyNoiseAmount, Props.gpuFogDetailNoiseAmount);
    vec4 p5 = vec4(Props.gpuFogSimpleAnimationX, Props.gpuFogSimpleAnimationY, Props.gpuFogSimpleAnimationZ, Props.gpuFogSimpleAnimationW);
    vec4 p6 = vec4(Props.gpuFogSimplexAnimationX, Props.gpuFogSimplexAnimationY, Props.gpuFogSimplexAnimationZ, Props.gpuFogSimplexAnimationW);
    vec4 p7 = vec4(Props.gpuFogVoronoiAnimationX, Props.gpuFogVoronoiAnimationY, Props.gpuFogVoronoiAnimationZ, Props.gpuFogVoronoiAnimationW);
    vec4 p8 = vec4(Props.gpuFogCameraDepthFadeRange, Props.gpuFogCameraDepthFadeOffset, 0.0, 0.0);

    float density = 1.0;
    float radial = 1.0;
#ifndef GE_USER_PARTICLE_TEXTURE
    // Without a sheet the procedural shape is the whole silhouette. Its density
    // noise is most of a smoke fragment's cost, so the silhouette comes first and a fragment outside
    // it stops there (unlit particles only, as above).
    vec2 centeredUv;
    radial = GPUFogShapeSilhouette(uv, centeredUv, timeSeconds, particleStableRandom, p0, p1, p2, p4, p5, p6);
#if !defined(GE_USER_PARTICLE_LIT)
    if (discardTransparent && GE_ParticleDrawsNothing(radial))
        discard;
#endif
    density = GPUFogShapeDensity(uv, centeredUv, timeSeconds, particleStableRandom, p0, p1, p2, p4, p5, p6, p7);
#endif

    float frames = clamp(Props.sheetFrames, 1.0, max(Props.sheetColumns * Props.sheetRows, 1.0));
    float framePosition = clamp(animation.y, 0.0, frames - 0.0001);
    float frame = floor(framePosition);
    float next = Props.sheetLoop > 0.5 ? mod(frame + 1.0, frames) : min(frame + 1.0, frames - 1.0);
    float blend = fract(framePosition);
#ifdef GE_USER_PARTICLE_TEXTURE
    vec4 albedo = ParticleSheetSample(albedoMap, sIn.uv0, frame, next, blend, footprint);
    // An authored sheet owns its silhouette and density. The procedural noise
    // remains available to emitters without a texture.
#else
    // Without a texture the material's albedo slot holds the default white, so the procedural
    // shape above is the whole silhouette and nothing is fetched.
    vec4 albedo = vec4(1.0);
#endif
    if (Props.wingFlutter > 0.5)
    {
        // The tint blends between the two wing colours by closeness to the sheet's corners at v = 0.
        float leftMask = 1.0 - clamp(distance(sIn.uv0, vec2(0,0)) - 0.5, 0.0, 1.0);
        float rightMask = 1.0 - clamp(distance(sIn.uv0, vec2(1,0)) - 0.5, 0.0, 1.0);
        float tintBlend = (leftMask - 0.3) / 1.2 + (rightMask - 0.3) / 1.2;
        albedo.rgb *= mix(Props.wingColor1.rgb, Props.wingColor2.rgb, tintBlend);
    }
    float surfaceFade = 1.0;
    float particleEyeDepth = abs(sIn.linearDepth);
    float surfaceDepthFade = max(p4.x, 0.0);
    if (surfaceDepthFade > 1e-4)
    {
        vec2 depthSize = vec2(textureSize(ge_sceneDepth, 0));
        vec2 screenUV = clamp(gl_FragCoord.xy / max(depthSize, vec2(1.0)), vec2(0.0), vec2(1.0));
        float sceneRawDepth = texelFetch(ge_sceneDepth, ivec2(gl_FragCoord.xy), 0).r;
        if (sceneRawDepth > 1e-6)
        {
            float sceneEyeDepth = GPUFogSceneEyeDepth(sceneRawDepth, screenUV);
            float fragmentEyeDepth = GPUFogSceneEyeDepth(gl_FragCoord.z, screenUV);
            surfaceFade = clamp(max(sceneEyeDepth - fragmentEyeDepth, 0.0) / surfaceDepthFade, 0.0, 1.0);
        }
    }
    float cameraFade = GPUFogCameraFade(particleEyeDepth, p8);

    float opacity = albedo.a;
    if (Props.alphaFromGrayscale > 0.5)
    {
        // Import-generated alpha uses source sRGB intensity, before decoding.
        vec3 sourceRGB = mix(albedo.rgb * 12.92,
            1.055 * pow(max(albedo.rgb, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055,
            step(vec3(0.0031308), albedo.rgb));
        opacity = dot(sourceRGB, vec3(1.0 / 3.0));
    }
    // Without an emission texture or a packed emission mask the whole particle emits; Emission
    // Intensity is 0 by default, so nothing glows until an author asks for it.
    vec3 emission = vec3(1.0);
    o.baseColor = albedo.rgb * Props.baseColor.rgb * sIn.vertexColor.rgb * lifetimeColor.rgb;
    o.normalWS = normalize(sIn.normalWS);
#ifdef GE_USER_PARTICLE_LIT
    o.particleBasis = sIn.particleBasis;
    o.particlePositive = vec3(1.0);
    o.particleNegative = vec3(1.0);
#ifdef GE_USER_SIX_WAY
    vec4 positiveSample = ParticleSheetSample(GE_USER_TEXTURE(positiveAxesMap), sIn.uv0, frame, next, blend, footprint);
    vec4 negativeSample = ParticleSheetSample(GE_USER_TEXTURE(negativeAxesMap), sIn.uv0, frame, next, blend, footprint);
    vec3 positive = max(positiveSample.rgb, vec3(0.0));
    vec3 negative = max(negativeSample.rgb, vec3(0.0));
#ifdef GE_USER_SIX_WAY_RGBA
    float emissionMask;
    GE_DecodeSixWayRGBA(positiveSample, negativeSample, positive, negative, opacity, emissionMask);
    emission = vec3(emissionMask);
    // Packed A owns opacity even without a separate color sheet.
    density = 1.0;
    radial = 1.0;
#endif
#ifdef GE_USER_SIX_WAY_TOP_LEFT_RIGHT_BOTTOM_BACK_FRONT
    GE_DecodeSixWayTopLeftRightBottomBackFront(positiveSample.rgb, negativeSample.rgb, positive, negative);
#endif
    float contrast = max(Props.sixWayStrength, 0.0);
    o.particlePositive = contrast > 0.0 ? pow(positive, vec3(contrast)) : vec3(1.0);
    o.particleNegative = contrast > 0.0 ? pow(negative, vec3(contrast)) : vec3(1.0);
#endif
    o.specularWeight = 0.0;
    o.roughness = 1.0;
    o.iblGroundDarkening = 0.0;
#endif
    // A separate RGB sheet replaces the monochrome packed mask, preserving
    // authored fire colors without adding or multiplying that mask twice.
#ifdef GE_USER_PARTICLE_EMISSION_TEXTURE
    emission = ParticleSheetSample(GE_USER_TEXTURE(emissionMap), sIn.uv0, frame, next, blend, footprint).rgb;
#endif
    o.emissive = GE_ParticleEmission(emission, Props.emissionColor, Props.emissionIntensity);
    float nearFade = 1.0;
#ifdef GE_USER_PARTICLE_BUFFER
    nearFade = sIn.particleNearFade;
#endif
    float nearFadeColorScale = GE_ParticleNearFadeColorScale(nearFade, Props.nearFadeColorExponent);
    o.emissive *= nearFadeColorScale;
    opacity = GE_ParticleCoverage(opacity);
    o.opacity = density * radial * surfaceFade * cameraFade * opacity * Props.baseColor.a * sIn.vertexColor.a * lifetimeColor.a;
    if (Props.hybridAlphaBlend > 0.5)
    {
        vec4 tint = Props.baseColor * sIn.vertexColor * lifetimeColor;
        float alphaFactor = 1.0 - clamp(max(max(tint.r, tint.g), tint.b) * 2.0 - 1.0, 0.0, 1.0);
        // Source uses One / OneMinusSrcAlpha: RGB includes particle alpha,
        // while texture RGB already supplies the luminous sprite silhouette.
        float fade = density * radial * surfaceFade * cameraFade;
        o.baseColor = albedo.rgb * tint.rgb * tint.a *
            mix(Props.hybridColorMultiplier, 2.0, alphaFactor) * fade;
        o.opacity = opacity * tint.a * alphaFactor * fade;
        // The hybrid colour already carries the near fade through tint.a, as its alpha does.
    }
    else
    {
        o.baseColor *= nearFadeColorScale;
    }
#if defined(GE_USER_PARTICLE_BUFFER) && !defined(GE_USER_PARTICLE_LIT)
    // Unlit particles are display-referred: colour and emission 1.0 draw as the display's
    // reference white at the view's exposure, so an unlit sprite reads the same in daylight and
    // at night. The vertex stage resolves the reciprocal of the view's exposure once per vertex
    // (particle_vertex.glsl). Lit particles stay scene-linear; the scene's lights set their brightness.
    o.baseColor *= sIn.particleColorScale;
    o.emissive *= sIn.particleColorScale;
#endif
#if !defined(GE_USER_PARTICLE_LIT)
    // Nothing after an unlit surface samples by derivatives or takes one.
    if (discardTransparent && GE_ParticleDrawsNothing(o.opacity))
        discard;
#endif
    return o;
}
