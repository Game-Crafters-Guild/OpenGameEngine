#version 450
#define OCEAN_MATERIAL_SET 0
#define OCEAN_MATERIAL_BINDING 18
#include "Ocean/ocean_material_profiles.glsl"

#include "Includes/screen_position.glsl"
#include "Includes/shadow_cascade_blend.glsl"
#include "Ocean/ocean_cascade_common.glsl"

// Underwater fullscreen composite. Runs at the post/composite phase when the
// camera is below (or straddling) the displaced ocean surface — the render node
// CPU-gates it. Per pixel it tests which side of the surface this view ray looks
// at, against the REAL FFT surface height (so the waterline conforms to the actual
// waves, not an analytic approximation): above the waterline it leaves the original
// frame untouched, below it applies the underwater look —
//   * per-channel Beer-Lambert fog that fades distant geometry into the deep body,
//   * single-scatter sun inscattering (Henyey-Greenstein), refracted into water,
//   * procedural god-ray shafts modulating the inscatter (clean-room Wicked port),
//   * lens-barrel + waterline-intersection screen distortion,
//   * a thin constant-width meniscus seam on the line.
//
// OceanUnderwater.cpp binds the scene, wave fields, volume/material tables and
// optional shadows. OceanSurface fields control extinction, caustics and scattering.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uScene;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(std140, set = 0, binding = 2) uniform OceanUnderwaterParams
{
    mat4 uInvViewProj;   // clip -> world
    mat4 uViewProj;      // world -> clip (god-ray sun projection)
    mat4 uView;          // world -> view (shadow cascade selection)
    vec4 uCameraPos;     // xyz world camera position
    vec4 uFogDensity;    // rgb = per-channel extinction; w = max fog distance (m)
    vec4 uFogFalloff;    // x = start distance, y = end distance, z = power, w = mode
    vec4 uDeepTint;      // rgb = deep underwater body colour; a unused
    vec4 uShallowTint;   // rgb = near-surface scatter colour; a = meniscus brightness
    vec4 uMisc;          // x = seaLevel, y = meniscusWidth, z = submergedDepth (m), w = time
    vec4 uSunDirection;  // xyz = toward the sun (world)
    vec4 uSunColor;      // rgb = light colour * intensity
    vec4 uFFTParams;     // x = legacy waveMode, y = fftCascadeCount, z = shapeWeight, w = maxVertical
    vec4 uLocalFFT;      // x = ready local FFT stream bit mask
    vec4 uLocalFFTMaskCascadeOriginScale[7];
    vec4 uLocalFFTMaskCascadeMeta; // x = lod count
    vec4 uShape;         // x = causticsEnable, y = iorWater, z = probe reach (m), w = waterline edge softness (m)
    vec4 uInscatter;     // x = enable, y = strength, z = HG phase g, w = camera-depth falloff
    vec4 uDistortion;    // x = enable, y = lens K1, z = lens K2, w = intersection strength
    vec4 uGodRays;       // x = mode (0 off,1 procedural), y = strength, z = density, w = secondary density
    vec4 uReflectedCaustics; // x = strength, y = max height above surface (m), z = falloff, w = pad
    vec4 uCaustics;      // scale, average, strength, focal depth
    vec4 uCausticsFocus; // depth of field, distortion strength/scale, pad
    vec4 uGerstner;      // legacy ABI pad
    vec4 uPortal;        // x = enabled, y = box count, z = exclusion count, w = polygon count
    vec4 uPortalExtra;   // x = portal occluder count, y = ribbon count, z = 1 when the camera is dry (portal-only)
    vec4 uPortalVolumeBoxes[16];    // 8 boxes: min.xyz, max.xyz pairs
    vec4 uPortalExclusionBoxes[8];  // 4 boxes: min.xyz, max.xyz pairs
    vec4 uPortalOccluderBoxes[16];  // 8 boxes: min.xyz, max.xyz pairs
    vec4 uPortalPolygons[36];       // 4 polygons: meta(surfaceY,depth,count,pad) + 8 xz points
    vec4 uWaves[32];     // legacy ABI pad
};

// FFT displacement cascades (height in .y). Tileable: layer c is a 0.5*2^c m patch,
// sampled Repeat. Bound descriptor-direct by OceanUnderwater.cpp (feature-owned).
layout(set = 0, binding = 3) uniform sampler2DArray uOceanDisplacement;

// Procedural caustics web (set0 b4): tileable, B = caustic intensity. Bound
// descriptor-direct from the feature; uShape.x gates use (a dummy 2D texture is
// bound when caustics are unavailable, so the static reference is always valid).
layout(set = 0, binding = 4) uniform sampler2D uOceanCaustics;

// Optional local spectrum displacement streams + per-stream local-FFT masks. When
// available, the RGBA mask pages blend waterline/god-ray surface sampling from
// the global FFT stream to the matching local FFT stream, mirroring the visible
// surface. Bindings 8/9 are reserved for the shadowed variant.
layout(set = 0, binding = 5) uniform sampler2DArray uOceanLocalDisplacement0;
layout(set = 0, binding = 6) uniform sampler2DArray uOceanLocalDisplacement1;
layout(set = 0, binding = 7) uniform sampler2DArray uOceanLocalDisplacement2;
layout(set = 0, binding = 10) uniform sampler2DArray uOceanLocalDisplacement3;
layout(set = 0, binding = 11) uniform sampler2DArray uOceanLocalFFTMask0;
layout(set = 0, binding = 12) uniform sampler2DArray uOceanLocalDisplacement4;
layout(set = 0, binding = 13) uniform sampler2DArray uOceanLocalDisplacement5;
layout(set = 0, binding = 14) uniform sampler2DArray uOceanLocalDisplacement6;
layout(set = 0, binding = 15) uniform sampler2DArray uOceanLocalDisplacement7;
layout(set = 0, binding = 16) uniform sampler2DArray uOceanLocalFFTMask1;

#ifdef OCEAN_UNDERWATER_SHADOWS
layout(set = 0, binding = 8, std140) uniform ShadowData
{
    mat4 ge_shadowVP[4];
    vec4 ge_shadowSplits;
    vec4 ge_shadowParams; // x=depthBias, y=normalBias, z=numCascades, w=maxShadowDistance
    vec4 ge_shadowDebug;
    vec4 ge_shadowPcss;
    vec4 ge_shadowPcssCascades[4];
    vec4 ge_shadowPcssPyramid[4];
};

layout(set = 0, binding = 9) uniform sampler2DArrayShadow ge_shadowMapArray;
#endif

const float PI = 3.14159265;
const int OCEAN_UNDERWATER_PORTAL_MAX_VOLUMES = 8;
const int OCEAN_UNDERWATER_PORTAL_MAX_EXCLUSIONS = 4;
const int OCEAN_UNDERWATER_PORTAL_MAX_OCCLUDERS = 8;
const int OCEAN_UNDERWATER_PORTAL_MAX_POLYGONS = 4;
const int OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS = 8;
const int OCEAN_UNDERWATER_PORTAL_POLYGON_STRIDE = 9;
const int OCEAN_UNDERWATER_LOCALFFT_MASK_LODS = 7;

// ViewportUV <-> Y-up NDC via the canonical screen_position.glsl helpers — one source
// of truth for the engine's fullscreen convention. The backend reconciles Y-up
// (Vulkan negative viewport == Metal native NDC), so these are correct on BOTH with
// no per-backend flip; do not reintroduce a local Y-flip here.
vec2 UvToNdc(vec2 uv) { return GE_ViewportUVToYUpNdc(uv); }
vec2 NdcToUv(vec2 ndc) { return GE_YUpNdcToViewportUV(ndc); }

// World position from a reverse-Z raw depth (near->1, far->0) via the true inverse.
vec3 WorldFromDepth(vec2 uv, float rawDepth)
{
    vec4 clip = vec4(UvToNdc(uv), rawDepth, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(abs(world.w), 1e-5);
}

float GerstnerHeight(vec2 worldXZ)
{
    float y = uMisc.x;
    int count = int(min(uGerstner.x, 16.0));
    for (int i = 0; i < count; ++i)
    {
        vec4 a = uWaves[i * 2];
        vec4 b = uWaves[i * 2 + 1];
        vec2 dir = a.xy;
        dir /= max(length(dir), 1.0e-4);
        float wl = max(a.w, 1e-3);
        float k = 6.2831853 / wl;
        float c = sqrt(9.81 / k) * b.y;
        float phase = k * dot(dir, worldXZ) + uMisc.w * (k * c);
        y += a.z * sin(phase);
    }
    return y;
}

uint UnderwaterLocalFFTMaskBits()
{
    return uint(max(uLocalFFT.x, 0.0) + 0.5);
}

vec4 UnderwaterSampleLocalFFTMaskTex(uint page, vec3 uvw)
{
    if (page == 0u)
        return textureLod(uOceanLocalFFTMask0, uvw, 0.0);
    return textureLod(uOceanLocalFFTMask1, uvw, 0.0);
}

vec4 UnderwaterSampleLocalFFTMask(vec2 worldXZ, uint page)
{
    if (UnderwaterLocalFFTMaskBits() == 0u)
        return vec4(0.0);
    int lodCount = min(int(uLocalFFTMaskCascadeMeta.x + 0.5), OCEAN_UNDERWATER_LOCALFFT_MASK_LODS);
    if (lodCount <= 0)
        return vec4(0.0);

    float res = float(textureSize(uOceanLocalFFTMask0, 0).x);
    for (int lod = 0; lod < OCEAN_UNDERWATER_LOCALFFT_MASK_LODS; ++lod)
    {
        if (lod >= lodCount)
            break;
        vec4 layer = uLocalFFTMaskCascadeOriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
        {
            return clamp(UnderwaterSampleLocalFFTMaskTex(page, vec3(uv, float(lod))), 0.0, 1.0);
        }
    }

    vec4 coarse = uLocalFFTMaskCascadeOriginScale[lodCount - 1];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return clamp(UnderwaterSampleLocalFFTMaskTex(
                     page, vec3(clamp(uv, 0.0, 1.0), float(lodCount - 1))),
                 0.0, 1.0);
}

float OceanFFTHeightAt(sampler2DArray displacementTex, vec2 worldXZ, float viewDist)
{
    int count = int(min(uFFTParams.y, 16.0));
    float h = 0.0;
    for (int c = 0; c < count; ++c)
    {
        float worldSize = 0.5 * float(1 << c);
        float fadeFar = worldSize * 60.0;
        float fade = 1.0 - smoothstep(fadeFar * 0.5, fadeFar, viewDist);
        if (fade <= 0.0)
            continue;
        vec2 uv = worldXZ / worldSize;
        h += textureLod(displacementTex, vec3(uv, float(c)), 0.0).y * fade;
    }
    return h;
}

float OceanLocalFFTHeightAt(vec2 worldXZ, float viewDist, uint stream)
{
    int count = int(min(uFFTParams.y, 16.0));
    float h = 0.0;
    for (int c = 0; c < count; ++c)
    {
        float worldSize = 0.5 * float(1 << c);
        float fadeFar = worldSize * 60.0;
        float fade = 1.0 - smoothstep(fadeFar * 0.5, fadeFar, viewDist);
        if (fade <= 0.0)
            continue;
        vec2 uv = worldXZ / worldSize;
        float localH = 0.0;
        if (stream == 0u)
            localH = textureLod(uOceanLocalDisplacement0, vec3(uv, float(c)), 0.0).y;
        else if (stream == 1u)
            localH = textureLod(uOceanLocalDisplacement1, vec3(uv, float(c)), 0.0).y;
        else if (stream == 2u)
            localH = textureLod(uOceanLocalDisplacement2, vec3(uv, float(c)), 0.0).y;
        else if (stream == 3u)
            localH = textureLod(uOceanLocalDisplacement3, vec3(uv, float(c)), 0.0).y;
        else if (stream == 4u)
            localH = textureLod(uOceanLocalDisplacement4, vec3(uv, float(c)), 0.0).y;
        else if (stream == 5u)
            localH = textureLod(uOceanLocalDisplacement5, vec3(uv, float(c)), 0.0).y;
        else if (stream == 6u)
            localH = textureLod(uOceanLocalDisplacement6, vec3(uv, float(c)), 0.0).y;
        else
            localH = textureLod(uOceanLocalDisplacement7, vec3(uv, float(c)), 0.0).y;
        h += localH * fade;
    }
    return h;
}

float OceanHeightAt(vec2 worldXZ, float viewDist)
{
    float seaLevel = uMisc.x;
    if (uFFTParams.x < 0.5 || uFFTParams.y < 0.5)
        return GerstnerHeight(worldXZ);

    float h = OceanFFTHeightAt(uOceanDisplacement, worldXZ, viewDist);
    uint localMask = UnderwaterLocalFFTMaskBits();
    if (localMask != 0u)
    {
        vec4 localWeights0 = UnderwaterSampleLocalFFTMask(worldXZ, 0u);
        if ((localMask & 1u) != 0u && localWeights0.x > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 0u), localWeights0.x);
        if ((localMask & 2u) != 0u && localWeights0.y > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 1u), localWeights0.y);
        if ((localMask & 4u) != 0u && localWeights0.z > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 2u), localWeights0.z);
        if ((localMask & 8u) != 0u && localWeights0.w > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 3u), localWeights0.w);

        vec4 localWeights1 = UnderwaterSampleLocalFFTMask(worldXZ, 1u);
        if ((localMask & 16u) != 0u && localWeights1.x > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 4u), localWeights1.x);
        if ((localMask & 32u) != 0u && localWeights1.y > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 5u), localWeights1.y);
        if ((localMask & 64u) != 0u && localWeights1.z > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 6u), localWeights1.z);
        if ((localMask & 128u) != 0u && localWeights1.w > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 7u), localWeights1.w);
    }
    h *= uFFTParams.z;                                       // shape weight
    h = clamp(h, -uFFTParams.w, uFFTParams.w);               // max vertical clamp
    return seaLevel + h;
}

// Integrate only the contiguous water segment containing the near plane.
// Upward rays stop at the displaced surface instead of fogging the sky behind it.
// The wave envelope bounds the search; a quadratic scan favours nearby crossings.
vec2 OceanWaterSegment(vec3 direction, float nearDistance, float sceneDistance)
{
    float cameraDepth = OceanHeightAt(uCameraPos.xz, 0.0) - uCameraPos.y;
    float start = cameraDepth >= 0.0 ? 0.0 : min(nearDistance, sceneDistance);
    float end = sceneDistance;
    if (direction.y > 0.001)
        end = min(end, max((uMisc.x + uFFTParams.w - uCameraPos.y) / direction.y, start));
    vec3 endPoint = uCameraPos.xyz + direction * end;
    float endDepth = OceanHeightAt(endPoint.xz, end) - endPoint.y;
    if (endDepth >= 0.0 || end <= start)
        return vec2(start, max(end - start, 0.0));

    float low = start, high = end;
    for (int i = 1; i <= 8; ++i)
    {
        float fraction = float(i) / 8.0;
        float distance = mix(start, end, fraction * fraction);
        vec3 p = uCameraPos.xyz + direction * distance;
        if (OceanHeightAt(p.xz, distance) <= p.y)
        {
            high = distance;
            break;
        }
        low = distance;
    }
    for (int i = 0; i < 5; ++i)
    {
        float distance = (low + high) * 0.5;
        vec3 p = uCameraPos.xyz + direction * distance;
        if (OceanHeightAt(p.xz, distance) > p.y)
            low = distance;
        else
            high = distance;
    }
    return vec2(start, max((low + high) * 0.5 - start, 0.0));
}

float HgPhase(float g, float c)
{
    float g2 = g * g;
    float d = 1.0 + g2 - 2.0 * g * c;
    return (1.0 - g2) / (4.0 * PI * pow(max(d, 1e-4), 1.5));
}

float UnderwaterFogAutoRange(vec3 density)
{
    float maxDensity = max(max(density.r, density.g), max(density.b, 1.0e-3));
    return 8.0 / maxDensity;
}

float UnderwaterFogRange(vec3 density)
{
    float startDist = max(uFogFalloff.x, 0.0);
    return uFogFalloff.y > startDist
        ? max(uFogFalloff.y - startDist, 1.0e-3)
        : UnderwaterFogAutoRange(density);
}

float UnderwaterFogShapedDistance(float waterDist, vec3 density)
{
    float startDist = max(uFogFalloff.x, 0.0);
    float clearDist = max(waterDist - startDist, 0.0);
    float power = max(uFogFalloff.z, 0.01);
    float mode = uFogFalloff.w;
    if (mode < 0.5)
        return pow(clearDist, power);

    float range = UnderwaterFogRange(density);
    float t = clamp(clearDist / range, 0.0, 1.0);
    if (mode > 1.5)
        t = t * t * (3.0 - 2.0 * t);
    t = pow(t, power);
    return t * range;
}

vec3 UnderwaterFogAlpha(vec3 density, float waterDist)
{
    float shapedDist = UnderwaterFogShapedDistance(waterDist, density);
    return vec3(1.0) - exp(-density * shapedDist);
}

float UnderwaterFogBodyMix(vec3 density, float waterDist)
{
    float range = UnderwaterFogRange(density);
    return clamp(UnderwaterFogShapedDistance(waterDist, density) / range, 0.0, 1.0);
}

float OceanHash12(vec2 p)
{
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float OceanValueNoise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = OceanHash12(i);
    float b = OceanHash12(i + vec2(1.0, 0.0));
    float c = OceanHash12(i + vec2(0.0, 1.0));
    float d = OceanHash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

// World-anchored surface mask for underwater shafts. This replaces the old
// screen-angle stripe overlay: each volumetric sample projects back to the water
// surface along the sun direction, then samples slow-moving cloud/caustic-like
// bands there. Bright gaps become sun beams; darker bands become occluded water.
float GodRaySurfaceMask(vec2 entryXZ, float t, float density)
{
    float scale = max(density, 1.0) * 0.0125;
    vec2 wind = normalize(vec2(0.82, 0.57));
    vec2 uv = entryXZ * scale + wind * t * 0.045;

    float broad = OceanValueNoise(uv);
    broad += 0.5 * OceanValueNoise(uv * 2.13 + vec2(9.7, -3.1) - wind.yx * t * 0.035);
    broad += 0.25 * OceanValueNoise(uv * 4.31 + vec2(-2.4, 6.8) + wind * t * 0.025);
    broad /= 1.75;

    float bands = sin(dot(entryXZ, normalize(vec2(0.62, -0.78))) * scale * 9.0 + t * 0.8);
    bands = bands * 0.5 + 0.5;
    float shafts = mix(broad, broad * bands, 0.55);
    return smoothstep(0.48, 0.86, shafts);
}

#ifdef OCEAN_UNDERWATER_SHADOWS
bool GodRayShadowUVInBounds(vec2 uv)
{
    return uv.x >= 0.0 && uv.x <= 1.0 && uv.y >= 0.0 && uv.y <= 1.0;
}

float GodRayShadowCompareTap(vec2 uv, float layer, float refDepth)
{
    if (!GodRayShadowUVInBounds(uv))
        return 1.0;
    return texture(ge_shadowMapArray, vec4(uv, layer, refDepth));
}

float SampleGodRayCascade(vec3 posWS, int cascadeIdx)
{
    vec4 shadowClip = ge_shadowVP[cascadeIdx] * vec4(posWS, 1.0);
    vec3 shadowNDC = shadowClip.xyz / max(abs(shadowClip.w), 1.0e-5);
    vec2 shadowUV = shadowNDC.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    float refDepthSpan = max(ge_shadowPcssCascades[0].y, 1.0e-3);
    float thisDepthSpan = max(ge_shadowPcssCascades[cascadeIdx].y, 1.0e-3);
    float biasScale = refDepthSpan / thisDepthSpan;
    float refDepth = shadowNDC.z + ge_shadowParams.x * biasScale;

    if (!GodRayShadowUVInBounds(shadowUV) || refDepth < 0.0 || refDepth > 1.0)
        return 1.0;

    float texelSize = 1.0 / float(textureSize(ge_shadowMapArray, 0).x);
    float layer = float(cascadeIdx);
    float shadow = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            vec2 offset = vec2(float(x), float(y)) * texelSize;
            shadow += GodRayShadowCompareTap(shadowUV + offset, layer, refDepth);
        }
    }
    return shadow * (1.0 / 9.0);
}

float SampleGodRayShadow(vec3 posWS, float linearDepth)
{
    int numCascades = int(ge_shadowParams.z + 0.5);
    if (numCascades <= 0 || linearDepth > ge_shadowParams.w)
        return 1.0;

    numCascades = clamp(numCascades, 1, 4);
    int cascadeIdx = numCascades - 1;
    for (int i = 0; i < numCascades; ++i)
    {
        if (linearDepth < ge_shadowSplits[i])
        {
            cascadeIdx = i;
            break;
        }
    }

    if (cascadeIdx < numCascades - 1)
    {
        float splitDist = ge_shadowSplits[cascadeIdx];
        float cascadeStart = (cascadeIdx == 0) ? 0.0 : ge_shadowSplits[cascadeIdx - 1];
        float cascadeRange = max(splitDist - cascadeStart, 1.0e-3);
        float blendBand = GE_CascadeBlendBand(cascadeRange);
        float distToSplit = splitDist - linearDepth;
        if (distToSplit < blendBand)
        {
            float t = smoothstep(0.0, 1.0, distToSplit / blendBand);
            float currentShadow = SampleGodRayCascade(posWS, cascadeIdx);
            float nextShadow = SampleGodRayCascade(posWS, cascadeIdx + 1);
            return mix(nextShadow, currentShadow, t);
        }
    }

    return SampleGodRayCascade(posWS, cascadeIdx);
}
#else
float SampleGodRayShadow(vec3 posWS, float linearDepth)
{
    return 1.0;
}
#endif

float GodRayViewDepth(vec3 posWS)
{
    return (uView * vec4(posWS, 1.0)).z;
}

float GodRayExtinction(vec3 density)
{
    vec3 e = max(density, vec3(0.001));
    return dot(e, vec3(0.2126, 0.7152, 0.0722));
}

bool RayBoxIntersect(vec3 ro, vec3 rd, vec3 bmin, vec3 bmax, out float tEnter, out float tExit)
{
    vec3 rdSafe = vec3(
        abs(rd.x) < 1.0e-6 ? (rd.x < 0.0 ? -1.0e-6 : 1.0e-6) : rd.x,
        abs(rd.y) < 1.0e-6 ? (rd.y < 0.0 ? -1.0e-6 : 1.0e-6) : rd.y,
        abs(rd.z) < 1.0e-6 ? (rd.z < 0.0 ? -1.0e-6 : 1.0e-6) : rd.z);
    vec3 invRd = 1.0 / rdSafe;
    vec3 t0 = (bmin - ro) * invRd;
    vec3 t1 = (bmax - ro) * invRd;
    vec3 tMin = min(t0, t1);
    vec3 tMax = max(t0, t1);
    tEnter = max(max(tMin.x, tMin.y), tMin.z);
    tExit = min(min(tMax.x, tMax.y), tMax.z);
    return tExit >= max(tEnter, 0.0);
}

float PortalCross2(vec2 a, vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

vec2 PortalPolygonPoint(int base, int index)
{
    return uPortalPolygons[base + 1 + index].xy;
}

bool PortalPointInPolygon(vec2 p, int base, int count)
{
    bool inside = false;
    int j = count - 1;
    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS; ++i)
    {
        if (i >= count)
            break;
        vec2 a = PortalPolygonPoint(base, i);
        vec2 b = PortalPolygonPoint(base, j);
        float denom = b.y - a.y;
        bool crosses = ((a.y > p.y) != (b.y > p.y)) &&
                       (p.x < (b.x - a.x) * (p.y - a.y) / denom + a.x);
        if (crosses)
            inside = !inside;
        j = i;
    }
    return inside;
}

bool RayPortalPolygonPrismIntersect(vec3 ro, vec3 rd, int polygonIndex, float sceneDist,
                                    out float tEnter, out float tExit, out float surfaceY)
{
    int base = polygonIndex * OCEAN_UNDERWATER_PORTAL_POLYGON_STRIDE;
    vec4 meta = uPortalPolygons[base];
    surfaceY = meta.x;
    float depth = max(meta.y, 0.0);
    int count = int(clamp(floor(meta.z + 0.5), 0.0, float(OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS)));
    if (count < 3 || depth <= 1.0e-4)
        return false;

    float topY = surfaceY;
    float bottomY = surfaceY - depth;
    float tMin = 0.0;
    float tMax = sceneDist;
    if (abs(rd.y) < 1.0e-6)
    {
        if (ro.y < bottomY || ro.y > topY)
            return false;
    }
    else
    {
        float ty0 = (bottomY - ro.y) / rd.y;
        float ty1 = (topY - ro.y) / rd.y;
        tMin = max(tMin, min(ty0, ty1));
        tMax = min(tMax, max(ty0, ty1));
    }
    if (tMax <= tMin)
        return false;

    vec2 ro2 = ro.xz;
    vec2 rd2 = rd.xz;
    if (dot(rd2, rd2) < 1.0e-10)
    {
        if (!PortalPointInPolygon(ro2, base, count))
            return false;
        tEnter = tMin;
        tExit = tMax;
        return true;
    }

    float crossings[OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS];
    int crossingCount = 0;
    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS; ++i)
    {
        if (i >= count)
            break;
        int j = i + 1;
        if (j >= count)
            j = 0;

        vec2 a = PortalPolygonPoint(base, i);
        vec2 b = PortalPolygonPoint(base, j);
        vec2 edge = b - a;
        float denom = PortalCross2(rd2, edge);
        if (abs(denom) < 1.0e-7)
            continue;

        vec2 rel = a - ro2;
        float t = PortalCross2(rel, edge) / denom;
        float u = PortalCross2(rel, rd2) / denom;
        if (u < -1.0e-4 || u > 1.0 + 1.0e-4 || t <= tMin + 1.0e-4 || t >= tMax - 1.0e-4)
            continue;

        bool duplicate = false;
        for (int k = 0; k < OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS; ++k)
        {
            if (k >= crossingCount)
                break;
            if (abs(crossings[k] - t) < 1.0e-3)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate && crossingCount < OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS)
            crossings[crossingCount++] = t;
    }

    for (int i = 1; i < OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS; ++i)
    {
        if (i >= crossingCount)
            break;
        float value = crossings[i];
        int j = i - 1;
        while (j >= 0 && crossings[j] > value)
        {
            crossings[j + 1] = crossings[j];
            --j;
        }
        crossings[j + 1] = value;
    }

    float prev = tMin;
    for (int i = 0; i <= OCEAN_UNDERWATER_PORTAL_MAX_POLYGON_POINTS; ++i)
    {
        float curr = (i < crossingCount) ? crossings[i] : tMax;
        if (curr > prev + 1.0e-4)
        {
            float mid = (prev + curr) * 0.5;
            if (PortalPointInPolygon(ro2 + rd2 * mid, base, count))
            {
                tEnter = prev;
                tExit = curr;
                return true;
            }
        }
        if (i >= crossingCount)
            break;
        prev = curr;
    }

    return false;
}

void TrimPortalSegmentByExclusions(vec3 ro, vec3 rd, inout float waterStart, inout float waterEnd)
{
    int count = int(min(uPortal.z, float(OCEAN_UNDERWATER_PORTAL_MAX_EXCLUSIONS)));
    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_EXCLUSIONS; ++i)
    {
        if (i >= count)
            break;
        vec3 bmin = uPortalExclusionBoxes[i * 2].xyz;
        vec3 bmax = uPortalExclusionBoxes[i * 2 + 1].xyz;
        float exStart;
        float exEnd;
        if (!RayBoxIntersect(ro, rd, bmin, bmax, exStart, exEnd))
            continue;

        exStart = max(exStart, waterStart);
        exEnd = min(exEnd, waterEnd);
        if (exEnd <= exStart)
            continue;

        if (exStart <= waterStart + 1.0e-3)
            waterStart = max(waterStart, exEnd);
        else
            waterEnd = min(waterEnd, exStart);
    }
}

void TrimPortalSegmentByOccluders(vec3 ro, vec3 rd, inout float waterStart, inout float waterEnd)
{
    int count = int(min(uPortalExtra.x, float(OCEAN_UNDERWATER_PORTAL_MAX_OCCLUDERS)));
    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_OCCLUDERS; ++i)
    {
        if (i >= count)
            break;
        vec3 bmin = uPortalOccluderBoxes[i * 2].xyz;
        vec3 bmax = uPortalOccluderBoxes[i * 2 + 1].xyz;
        float occStart;
        float occEnd;
        if (!RayBoxIntersect(ro, rd, bmin, bmax, occStart, occEnd))
            continue;

        occStart = max(occStart, 0.0);
        occEnd = max(occEnd, 0.0);
        if (occEnd <= occStart || occStart >= waterEnd)
            continue;

        if (occEnd <= waterStart + 1.0e-3)
            waterEnd = min(waterEnd, occStart);
        else if (occStart <= waterStart + 1.0e-3)
            waterStart = max(waterStart, occEnd);
        else
            waterEnd = min(waterEnd, occStart);
    }
}

struct RibbonTriangle
{
    vec4 a;
    vec4 b;
    vec4 c;
    vec4 flowA;
    vec4 flowB;
    vec4 flowC;
    vec4 color;
    vec4 values;
    uvec4 meta;
};
layout(std430, set = 0, binding = 17) readonly buffer OceanUnderwaterRibbons
{
    RibbonTriangle ribbons[];
};
bool RibbonClipPlane(float value, float rate, inout float start, inout float end)
{
    if (abs(rate) < 1e-7)
        return value >= 0;
    float t = -value / rate;
    if (rate > 0)
        start = max(start, t);
    else
        end = min(end, t);
    return end > start;
}
bool RayRibbonPrism(vec3 ro, vec3 rd, RibbonTriangle t, float sceneDist, out float start, out float end,
                    out float surface)
{
    vec2 b = t.b.xy - t.a.xy, c = t.c.xy - t.a.xy, p = ro.xz - t.a.xy;
    float det = b.x * c.y - b.y * c.x;
    if (abs(det) < 1e-8)
        return false;
    vec3 w, d;
    w.y = (p.x * c.y - p.y * c.x) / det;
    w.z = (b.x * p.y - b.y * p.x) / det;
    w.x = 1 - w.y - w.z;
    d.y = (rd.x * c.y - rd.z * c.x) / det;
    d.z = (b.x * rd.z - b.y * rd.x) / det;
    d.x = -d.y - d.z;
    start = 0;
    end = sceneDist;
    for (int i = 0; i < 3; ++i)
        if (!RibbonClipPlane(w[i], d[i], start, end))
            return false;
    // The triangle reaches past the true edge by an apron; the cross-ribbon
    // coordinate is linear across it and within [-1, 1] inside the true edge.
    vec3 crossCoordinates = vec3(t.a.z, t.b.z, t.c.z);
    float crossAt = dot(w, crossCoordinates), crossRate = dot(d, crossCoordinates);
    if (!RibbonClipPlane(1 - crossAt, -crossRate, start, end) || !RibbonClipPlane(1 + crossAt, crossRate, start, end))
        return false;
    vec3 heights = vec3(t.a.w, t.b.w, t.c.w);
    float height = dot(w, heights), slope = dot(d, heights);
    if (!RibbonClipPlane(height - ro.y, slope - rd.y, start, end))
        return false;
    if (!RibbonClipPlane(ro.y - height + t.values.w, rd.y - slope, start, end))
        return false;
    surface = height + slope * start;
    return end > start;
}

vec4 UnderwaterPortalHit(vec3 viewDir, float sceneDist)
{
    if (uPortal.x < 0.5)
        return vec4(0.0);

    vec3 ro = uCameraPos.xyz;
    int count = int(min(uPortal.y, float(OCEAN_UNDERWATER_PORTAL_MAX_VOLUMES)));
    float bestStart = 1.0e20;
    float bestEnd = 0.0;
    float bestSurfaceY = uMisc.x;

    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_VOLUMES; ++i)
    {
        if (i >= count)
            break;
        vec3 bmin = uPortalVolumeBoxes[i * 2].xyz;
        vec3 bmax = uPortalVolumeBoxes[i * 2 + 1].xyz;
        float tStart;
        float tEnd;
        if (!RayBoxIntersect(ro, viewDir, bmin, bmax, tStart, tEnd))
            continue;

        tStart = max(tStart, 0.0);
        tEnd = min(tEnd, sceneDist);
        TrimPortalSegmentByExclusions(ro, viewDir, tStart, tEnd);
        TrimPortalSegmentByOccluders(ro, viewDir, tStart, tEnd);
        if (tEnd <= tStart || tStart >= bestStart)
            continue;

        bestStart = tStart;
        bestEnd = tEnd;
        bestSurfaceY = bmax.y;
    }

    int polygonCount = int(min(uPortal.w, float(OCEAN_UNDERWATER_PORTAL_MAX_POLYGONS)));
    for (int i = 0; i < OCEAN_UNDERWATER_PORTAL_MAX_POLYGONS; ++i)
    {
        if (i >= polygonCount)
            break;

        float tStart;
        float tEnd;
        float surfaceY;
        if (!RayPortalPolygonPrismIntersect(ro, viewDir, i, sceneDist, tStart, tEnd, surfaceY))
            continue;

        TrimPortalSegmentByExclusions(ro, viewDir, tStart, tEnd);
        TrimPortalSegmentByOccluders(ro, viewDir, tStart, tEnd);
        if (tEnd <= tStart || tStart >= bestStart)
            continue;

        bestStart = tStart;
        bestEnd = tEnd;
        bestSurfaceY = surfaceY;
    }

    // Find the first ribbon interval through a stackless bounding-volume tree.
    // Branch records share the triangle buffer and carry an absolute escape index.
    uint ribbonCount = uint(uPortalExtra.y);
    float ribbonStart = 1e20, ribbonEnd = 0, ribbonSurface = uMisc.x;
    for (uint i = 0u; i < ribbonCount;)
    {
        RibbonTriangle t = ribbons[i];
        if ((t.meta.x & 0x80000000u) != 0u)
        {
            float nearT, farT;
            bool hit = RayBoxIntersect(ro, viewDir, t.a.xyz, t.b.xyz, nearT, farT);
            i = (!hit || farT < 0 || nearT > min(ribbonStart, sceneDist)) ? t.meta.y : i + 1u;
            continue;
        }
        ++i;
        float begin, end, surface;
        if (!RayRibbonPrism(ro, viewDir, t, sceneDist, begin, end, surface))
            continue;
        TrimPortalSegmentByExclusions(ro, viewDir, begin, end);
        TrimPortalSegmentByOccluders(ro, viewDir, begin, end);
        if (end <= begin || begin >= ribbonStart)
            continue;
        ribbonStart = begin;
        ribbonEnd = end;
        ribbonSurface = surface;
    }
    // Union touching intervals instead of summing triangle thickness. This avoids
    // doubled fog at joints and does not count air gaps between river crossings.
    // BVH pruning limits follow-up visits to the growing contiguous segment.
    if (ribbonEnd > ribbonStart)
        for (uint iteration = 0u; iteration < ribbonCount; ++iteration)
        {
            float previousEnd = ribbonEnd;
            for (uint i = 0u; i < ribbonCount;)
            {
                RibbonTriangle t = ribbons[i];
                if ((t.meta.x & 0x80000000u) != 0u)
                {
                    float nearT, farT;
                    bool hit = RayBoxIntersect(ro, viewDir, t.a.xyz, t.b.xyz, nearT, farT);
                    i = (!hit || farT < ribbonStart || nearT > ribbonEnd + 0.001) ? t.meta.y : i + 1u;
                    continue;
                }
                ++i;
                float begin, end, surface;
                if (!RayRibbonPrism(ro, viewDir, t, sceneDist, begin, end, surface))
                    continue;
                TrimPortalSegmentByExclusions(ro, viewDir, begin, end);
                TrimPortalSegmentByOccluders(ro, viewDir, begin, end);
                if (end > begin && begin <= ribbonEnd + 0.001 && end >= ribbonStart)
                    ribbonEnd = max(ribbonEnd, end);
            }
            if (ribbonEnd <= previousEnd + 1e-5)
                break;
        }
    if (ribbonEnd > ribbonStart && ribbonStart < bestStart)
    {
        bestStart = ribbonStart;
        bestEnd = ribbonEnd;
        bestSurfaceY = ribbonSurface;
    }
    float waterDist = max(bestEnd - bestStart, 0.0);
    float mask = smoothstep(0.0, 0.25, waterDist);
    return vec4(mask, bestStart, waterDist, bestSurfaceY);
}

vec3 IntegrateUnderwaterGodRays(vec3 viewDir, float waterStart, float waterDist,
                                float belowMask, float surfaceY, float usePortalSurface, vec3 opticalFog)
{
    vec3 sunDirW = normalize(uSunDirection.xyz);
    float sunAbove = smoothstep(0.02, 0.30, sunDirW.y);
    if (belowMask <= 0.001 || sunAbove <= 0.001 || uGodRays.y <= 0.0)
        return vec3(0.0);

    const int sampleCount = 16;
    float maxDist = min(waterDist, 80.0);
    if (maxDist <= 1.0e-3)
        return vec3(0.0);
    float stepLen = maxDist / float(sampleCount);
    float jitter = OceanHash12(floor(gl_FragCoord.xy));
    vec3 lightDir = -refract(-sunDirW, vec3(0.0, 1.0, 0.0), 1.0 / max(uShape.y, 1.0));
    float viewTowardSun = clamp(dot(viewDir, lightDir), -1.0, 1.0);
    float phase = HgPhase(clamp(max(uInscatter.z, 0.35), -0.8, 0.85), viewTowardSun);
    float extinction = GodRayExtinction(opticalFog);
    float viewExtinction = max(extinction * 0.75, 0.018);
    float lightExtinction = max(extinction * 1.15, 0.026);

    vec3 sum = vec3(0.0);
    for (int i = 0; i < sampleCount; ++i)
    {
        float sampleT = (float(i) + 0.5 + jitter * 0.65) / float(sampleCount);
        float localS = sampleT * maxDist;
        float s = waterStart + localS;
        vec3 p = uCameraPos.xyz + viewDir * s;
        float surfaceH = surfaceY;
        if (usePortalSurface < 0.5)
            surfaceH = OceanHeightAt(p.xz, s);
        float depthBelow = max(surfaceH - p.y, 0.0);
        float toSurface = depthBelow / max(lightDir.y, 0.08);
        vec2 entryXZ = p.xz + lightDir.xz * toSurface;

        float mask = GodRaySurfaceMask(entryXZ, uMisc.w, max(uGodRays.z, 1.0));
        float depthBuild = 1.0 - exp(-depthBelow * 0.55);
        float viewTransmittance = exp(-localS * viewExtinction);
        float lightTransmittance = exp(-toSurface * lightExtinction);
        float shadow = SampleGodRayShadow(p, GodRayViewDepth(p));
        float surfaceModulation = mix(0.58, 1.18, mask);
        float shadowVisibility = mix(0.16, 1.0, smoothstep(0.03, 0.92, shadow));
        float marchWindow = smoothstep(0.00, 0.08, sampleT) * (1.0 - smoothstep(0.86, 1.00, sampleT));
        float local = surfaceModulation * shadowVisibility * depthBuild
                    * viewTransmittance * lightTransmittance * marchWindow;
        sum += uSunColor.rgb * local * phase;
    }

    float forwardBoost = mix(0.55, 1.35, smoothstep(0.0, 0.85, viewTowardSun));
    float waterClarity = exp(-extinction * min(maxDist, 30.0) * 0.18);
    float energy = mix(0.75, 1.15, waterClarity);
    return sum * (uGodRays.y * sunAbove * belowMask * stepLen * 0.22 * forwardBoost * energy);
}

void main()
{
    vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    // uScene (the scene copy) and uDepth (the resolved depth) have one mip, so the
    // explicit-LOD fetches below read exactly what texture() would, and stay legal
    // inside the per-quad branches.
    vec3 sceneAtPixel = textureLod(uScene, uv, 0.0).rgb;

    // A dry camera sees water only through a portal volume, and its lens is off
    // (FillUnderwaterParams), so this ray is the one the full path shades. A pixel
    // whose ray crosses no volume before its scene depth keeps the scene.
    bool dryCamera = uPortalExtra.z > 0.5;
    vec4 dryPortalHit = vec4(0.0);
    if (dryCamera)
    {
        vec3 dryViewDir = normalize(WorldFromDepth(uv, 1.0e-4) - uCameraPos.xyz);
        float dryDepth = textureLod(uDepth, uv, 0.0).r;
        float drySceneDist = uFogDensity.w;
        if (dryDepth > 1e-6)
            drySceneDist = length(WorldFromDepth(uv, dryDepth) - uCameraPos.xyz);
        dryPortalHit = UnderwaterPortalHit(dryViewDir, drySceneDist);
    }
    float pixelSeesWater = (!dryCamera || dryPortalHit.x > 0.001) ? 1.0 : 0.0;

    // The shading takes screen-space derivatives, which need every lane of the 2x2
    // quad, so it is skipped only where no pixel of the quad sees water. Fine
    // derivatives of a 0/1 value give the row maximum, then the quad maximum,
    // identical in all four lanes. The work runs in three branches on that quad
    // decision; the derivatives sit between them, in uniform control flow, and a
    // skipped quad pays only for them.
    float rowSeesWater = min(pixelSeesWater + abs(dFdxFine(pixelSeesWater)), 1.0);
    bool shadeQuad = rowSeesWater + abs(dFdyFine(rowSeesWater)) > 0.5;

    // --- Phase 1: the per-pixel waterline against the REAL displaced surface ---
    vec3 viewDir = vec3(0.0, 0.0, 1.0);
    float reach = 0.01;
    float oceanH = 0.0;
    float signedDepth = -1.0;                                 // > 0 => underwater
    vec2 uvUW = uv;
    if (shadeQuad)
    {
        // This pixel's view ray (un-project a far point and subtract the camera).
        viewDir = normalize(WorldFromDepth(uv, 1.0e-4) - uCameraPos.xyz);
        // Classify at the real near plane so shallow submersion keeps the upward
        // view underwater until the lens itself crosses the surface.
        reach = max(length(WorldFromDepth(uv, 1.0) - uCameraPos.xyz), 0.01);
        vec3 probe = uCameraPos.xyz + viewDir * reach;
        oceanH = OceanHeightAt(probe.xz, reach);
        signedDepth = oceanH - probe.y;                       // pre-distort

        // --- Screen distortion: intersection refraction pinch (Wicked INTERSECTION_DISTORT)
        // then Brown-Conrady barrel. After the pinch, RECOMPUTE the probe from the bent ray
        // (as Wicked does) so the waterline mask + fog track the distorted boundary. ---
        if (uDistortion.x > 0.5)
        {
            vec2 ndc = UvToNdc(uv);
            float blend = exp(-abs(signedDepth) / max(reach, 0.01) * 50.0); // 50: how tightly the pinch hugs the contact
            ndc *= mix(1.0, mix(1.0, 0.92, clamp(uDistortion.w, 0.0, 1.0)), blend); // pinch toward centre
            float r2 = dot(ndc, ndc) * 0.9;
            ndc *= 1.0 + uDistortion.y * r2 + uDistortion.z * r2 * r2;             // barrel lens
            uvUW = clamp(NdcToUv(ndc), 0.0, 1.0);
            viewDir = normalize(WorldFromDepth(uvUW, 1.0e-4) - uCameraPos.xyz);
            reach = max(length(WorldFromDepth(uvUW, 1.0) - uCameraPos.xyz), 0.01);
            probe = uCameraPos.xyz + viewDir * reach;
            oceanH = OceanHeightAt(probe.xz, reach);
            signedDepth = oceanH - probe.y;                                        // recomputed for the mask
        }
    }

    float signedDepthWidth = fwidth(signedDepth);
    vec2 edgeGrad = vec2(dFdx(signedDepth), dFdy(signedDepth));

    // --- Phase 2: the waterline mask, the refracted scene sample and the water segment ---
    vec3 originalColor = sceneAtPixel;                        // above-water: untouched away from the seam
    vec3 scene = sceneAtPixel;
    float rawDepth = 0.0;
    float sceneDist = uFogDensity.w;
    vec3 worldPos = uCameraPos.xyz;
    float belowMask = 0.0;
    float seamProximity = 0.0;
    float ripple = 0.0;
    vec4 portalHit = dryPortalHit;
    vec3 opticalFog = uFogDensity.rgb, opticalShallow = uShallowTint.rgb, opticalDeep = uDeepTint.rgb;
    float waterStart = 0.0;
    float waterDist = 0.0;
    float activeSurfaceH = oceanH;
    float portalSurface = 0.0;
    vec3 underwaterSun = vec3(0.0, 1.0, 0.0);
    float depthBelow = 0.0;
    float heightAbove = 0.0;
    vec2 causticUV = vec2(0.0);
    if (shadeQuad)
    {
        // Waterline mask + refraction. The transition should feel like a lensing
        // boundary, not a painted solid stripe: soften over a small world-space band and
        // bend both sides of the scene sample along the screen-space waterline normal.
        // Retain the authored transition width at a one-meter reference reach.
        // A short near plane must not spread its default feather over the whole frame.
        float edgeSoftness = max(uShape.w * reach, 0.00005);
        float edgeAa = max(signedDepthWidth * 1.5, 1.0e-4);
        belowMask = smoothstep(-edgeSoftness - edgeAa, edgeSoftness + edgeAa, signedDepth);
        seamProximity = 1.0 - smoothstep(0.0, edgeSoftness * 2.5 + edgeAa, abs(signedDepth));
        vec2 edgeNormal = dot(edgeGrad, edgeGrad) > 1.0e-8 ? normalize(edgeGrad) : vec2(0.0, 1.0);
        vec2 edgeTangent = vec2(-edgeNormal.y, edgeNormal.x);
        ripple = 0.55 * sin(dot(gl_FragCoord.xy, vec2(0.071, 0.033)) + uMisc.w * 2.1)
               + 0.45 * sin(dot(gl_FragCoord.xy, vec2(-0.041, 0.097)) - uMisc.w * 1.4);
        float refractStrength = seamProximity * clamp(uDistortion.w, 0.0, 2.0) * step(0.5, uDistortion.x);
        vec2 seamOffset = (edgeNormal * (0.45 + 0.35 * ripple) + edgeTangent * ripple * 0.55)
                        * refractStrength * 3.0 / vec2(textureSize(uScene, 0));
        vec2 sceneUV = clamp(mix(uvUW, uvUW + seamOffset, seamProximity), vec2(0.0), vec2(1.0));
        originalColor = mix(originalColor, textureLod(uScene, sceneUV, 0.0).rgb, seamProximity * 0.65);

        viewDir = normalize(WorldFromDepth(sceneUV, 1.0e-4) - uCameraPos.xyz);
        scene = textureLod(uScene, sceneUV, 0.0).rgb;
        rawDepth = textureLod(uDepth, sceneUV, 0.0).r;
        worldPos = uCameraPos.xyz + viewDir * uFogDensity.w;
        if (rawDepth > 1e-6)
        {
            worldPos = WorldFromDepth(sceneUV, rawDepth);
            // Extinction is measured through water, not through the air before a
            // river. Clamping the whole ray at the fog range cuts distant caustics
            // and bounded volumes off along a visible camera-centered arc.
            sceneDist = length(worldPos - uCameraPos.xyz);
        }

        vec2 waterSegment = uPortalExtra.y > 0.0 ? vec2(0.0, sceneDist)
            : OceanWaterSegment(viewDir, reach, sceneDist);
        if (!dryCamera)
            portalHit = UnderwaterPortalHit(viewDir, sceneDist);
        waterStart = waterSegment.x;
        waterDist = waterSegment.y;
        if (uPortalExtra.y > 0.0)
        {
            belowMask = portalHit.x;
            originalColor = mix(sceneAtPixel, originalColor, portalHit.x);
        }
        if (portalHit.x > 0.001)
        {
            belowMask = max(belowMask, portalHit.x);
            waterStart = portalHit.y;
            waterDist = portalHit.z;
            activeSurfaceH = portalHit.w;
            portalSurface = 1.0;
        }

        // The compat profile carries no water-body materials: ocean_material_profiles.glsl
        // compiles their buffer out there and OceanFindMaterial finds none.
#if !defined(GE_COMPAT_PROFILE)
        int waterMaterial = OceanFindMaterial((uCameraPos.xyz + viewDir * (waterStart + 0.1)).xz);
        if (waterMaterial >= 0)
        {
            opticalFog = oceanMaterials[waterMaterial].fog.rgb;
            opticalShallow = oceanMaterials[waterMaterial].shallow.rgb;
            opticalDeep = oceanMaterials[waterMaterial].deep.rgb;
        }
#endif

        vec3 sunDir = normalize(uSunDirection.xyz);
        underwaterSun = -refract(-sunDir, vec3(0, 1, 0), 1.0 / max(uShape.y, 1.0));
        float receiverSurface = activeSurfaceH;
        if (portalSurface < 0.5)
            receiverSurface = OceanHeightAt(worldPos.xz, sceneDist);
        depthBelow = max(receiverSurface - worldPos.y, 0.0);
        heightAbove = max(worldPos.y - receiverSurface, 0.0);
        vec2 basePos = worldPos.xz + underwaterSun.xz * (depthBelow - heightAbove) / max(underwaterSun.y, 0.1);
        float invScale = 1.0 / max(uCaustics.x, 0.01);
        float t = uMisc.w;
        vec2 distortion = vec2(sin(basePos.y / uCausticsFocus.z + t * 0.23),
                               cos(basePos.x / uCausticsFocus.z - t * 0.19)) * uCausticsFocus.y;
        causticUV = basePos * invScale + distortion;
    }

    // Reconstruct a receiver normal before divergent flow. Orient it toward the
    // camera, which rejects downward-facing ceilings while keeping the seabed lit.
    vec3 worldDx = dFdx(worldPos), worldDy = dFdy(worldPos);
    vec2 causticDx = dFdx(causticUV), causticDy = dFdy(causticUV);

    // --- Phase 3: caustics, fog, in-scatter, god rays and the meniscus ---
    vec3 result = sceneAtPixel;
    if (shadeQuad)
    {
        vec3 receiverNormal = cross(worldDx, worldDy);
        receiverNormal /= max(length(receiverNormal), 1e-6);
        if (dot(receiverNormal, -viewDir) < 0.0)
            receiverNormal = -receiverNormal;
        vec3 sunDir = normalize(uSunDirection.xyz);
        float t = uMisc.w;
        vec2 textureExtent = vec2(textureSize(uOceanCaustics, 0));
        vec2 footprintX = causticDx * textureExtent;
        vec2 footprintY = causticDy * textureExtent;
        float footprintLod = 0.5 * log2(max(max(dot(footprintX, footprintX), dot(footprintY, footprintY)), 1.0));
        float focalLod = abs(depthBelow - uCaustics.w) / max(uCausticsFocus.x, 0.001);
        float causticLod = max(footprintLod, focalLod);
        if (uShape.x > 0.5 && rawDepth > 1e-6 && belowMask > 0.01 && sunDir.y > 0.0)
        {
            float a = textureLod(uOceanCaustics, causticUV + vec2(0.044 * t + 17.16, -0.169 * t), causticLod).b;
            float b = textureLod(uOceanCaustics, 1.37 * causticUV + vec2(0.248 * t, 0.117 * t), causticLod + 0.45).b;
            float contrast = (a * b - uCaustics.y) * uCaustics.z;
            float belowWeight = exp(-depthBelow * max(dot(opticalFog, vec3(0.333)), 0.01)) * step(0.001, depthBelow);
            // A ray can cross a river and reach dry ground beyond its bank. Light
            // only receivers still inside the intersected water interval.
            if (portalSurface > 0.5)
                belowWeight *= step(length(worldPos - uCameraPos.xyz), waterStart + waterDist + 0.03);
            float reflectedWeight = exp(-heightAbove * max(uReflectedCaustics.z, 0.01))
                * (1.0 - smoothstep(0.0, max(uReflectedCaustics.y, 0.001), heightAbove)) * step(0.001, heightAbove);
            float receiverFacing = max(dot(receiverNormal, underwaterSun), 0.0);
            float reflectedFacing = max(dot(receiverNormal, -underwaterSun), 0.0);
            float illumination = smoothstep(0.0, 0.15, sunDir.y)
                * SampleGodRayShadow(worldPos + receiverNormal * 0.03, GodRayViewDepth(worldPos));
            float weight = belowWeight * receiverFacing + reflectedWeight * reflectedFacing * uReflectedCaustics.x;
            scene *= max(1.0 + contrast * weight * illumination * belowMask, 0.0);
        }

        // --- Fog from below (per-channel Beer-Lambert with authorable falloff shape) ---
        vec3 fogAlpha = UnderwaterFogAlpha(opticalFog, waterDist);
        float nearMix = UnderwaterFogBodyMix(opticalFog, waterDist);
        float nearClarity = smoothstep(0.0, max(uShape.w * 3.0, 0.15), waterDist);
        fogAlpha *= nearClarity;
        nearMix *= nearClarity;
        vec3 body = mix(opticalShallow, opticalDeep, nearMix);

        // --- Single-scatter sun inscattering (Henyey-Greenstein) + god rays ---
        vec3 fogColor = body;
        if (uInscatter.x > 0.5)
        {
            vec3 sunDirW = normalize(uSunDirection.xyz);
            // Refract the sun into the water (Snell) for the in-scatter phase term.
            vec3 Ldir = refract(-sunDirW, vec3(0.0, 1.0, 0.0), 1.0 / max(uShape.y, 1.0));
            float cosT = dot(viewDir, -Ldir);
            float camDepth = max(uMisc.z, 0.0);
            vec3 insc = uSunColor.rgb * HgPhase(uInscatter.z, cosT) * (vec3(1.0) - clamp(opticalFog, 0.0, 1.0)) *
                        exp(-camDepth * max(uInscatter.w, 0.0)) * smoothstep(0.0, 0.15, sunDirW.y);
            fogColor += max(insc, vec3(0.0)) * uInscatter.y;
        }

        vec3 fogged = mix(scene, fogColor, clamp(fogAlpha, 0.0, 1.0));

        // Above the wavy line keep the untouched frame; below, the underwater look.
        result = mix(originalColor, fogged, belowMask);

        // --- God rays (additive, underwater) ---
        // Short volumetric integration, Wicked-style: march along the view ray and
        // accumulate sun scatter. The shaft mask is world-anchored at the water surface,
        // so the beams stay in the volume instead of sticking to screen angle.
        if (uGodRays.x > 0.5)
        {
            result += IntegrateUnderwaterGodRays(viewDir, waterStart, waterDist, belowMask,
                                                 activeSurfaceH, portalSurface, opticalFog);
        }

        // --- Meniscus seam (screen-sized via fwidth, so the inspector width fattens the
        // band without blooming wildly at grazing/near-surface angles) ---
        // Subtle refractive meniscus highlight on the seam, broken up by the same ripple
        // used for the waterline distortion so wide settings still read watery, not drawn.
        float menis = max(uMisc.y, 0.0);
        if (menis > 1e-4)
        {
            float aa = max(signedDepthWidth, 1e-5);
            float menis01 = clamp(menis / 0.5, 0.0, 1.0);
            float widthPx = 0.5 + menis * 12.0;
            float seam = 1.0 - smoothstep(0.0, widthPx * aa, abs(signedDepth));
            float breakup = smoothstep(-0.65, 0.55, ripple);
            seam *= mix(0.35, 1.0, breakup) * seamProximity;
            if (uPortalExtra.y > 0.0)
                seam *= portalHit.x;
            vec3 meniscusCol = mix(result, opticalShallow, 0.35) + vec3(0.04);
            result = mix(result, meniscusCol,
                         clamp(seam * uShallowTint.a * mix(0.12, 0.22, menis01), 0.0, 1.0));
        }
    }

    outColor = vec4(pixelSeesWater > 0.5 ? result : sceneAtPixel, 1.0);
}
