#version 450

#include "Includes/screen_position.glsl"
#include "Ocean/ocean_cascade_common.glsl"

// Above-water reflected caustics from the ocean surface, composited over the scene.
// The filename is historical; this shader no longer contains the old radial blur.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uScene; // scene snapshot
layout(set = 0, binding = 2) uniform sampler2D uDepth; // resolved scene depth (sky mask)
layout(set = 0, binding = 3) uniform sampler2D uOceanCaustics;
layout(set = 0, binding = 4) uniform sampler2DArray uOceanDisplacement;
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

layout(std140, set = 0, binding = 1) uniform OceanReflectedCausticsParams
{
    mat4 uInvViewProj;
    vec4 uCameraPos;
    vec4 uTime;      // x = time
    vec4 uSunDirection; // xyz = toward the sun
    vec4 uOcean;     // x = seaLevel, y = causticsAvailable, z = surface displacement bound, w = minimum reflected rise (kReflectedCausticsMinRise)
    vec4 uCaustics;  // x = scale, y = average, z = strength, w = pad
    vec4 uReflectedCaustics; // x = strength, y = height above surface, z = falloff, w = pad
    vec4 uFFTParams; // x = legacy waveMode, y = fftCascadeCount, z = shapeWeight, w = maxVertical
    vec4 uLocalFFT;  // x = ready local FFT stream bit mask
    vec4 uLocalFFTMaskCascadeOriginScale[7];
    vec4 uLocalFFTMaskCascadeMeta; // x = lod count
    vec4 uGerstner;  // legacy ABI pad
    vec4 uWaves[32]; // legacy ABI pad
};

const int OCEAN_REFLECTED_CAUSTICS_LOCALFFT_MASK_LODS = 7;

vec3 WorldFromDepth(vec2 uv, float rawDepth)
{
    vec2 ndc = GE_ViewportUVToYUpNdc(uv);
    vec4 clip = vec4(ndc, rawDepth, 1.0);
    vec4 world = uInvViewProj * clip;
    return world.xyz / max(abs(world.w), 1e-5);
}

float GerstnerHeight(vec2 worldXZ)
{
    float y = uOcean.x;
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
        float phase = k * dot(dir, worldXZ) + uTime.x * (k * c);
        y += a.z * sin(phase);
    }
    return y;
}

uint LocalFFTMaskBits()
{
    return uint(max(uLocalFFT.x, 0.0) + 0.5);
}

vec4 SampleLocalFFTMaskTex(uint page, vec3 uvw)
{
    if (page == 0u)
        return textureLod(uOceanLocalFFTMask0, uvw, 0.0);
    return textureLod(uOceanLocalFFTMask1, uvw, 0.0);
}

vec4 SampleLocalFFTMask(vec2 worldXZ, uint page)
{
    if (LocalFFTMaskBits() == 0u)
        return vec4(0.0);
    int lodCount =
        min(int(uLocalFFTMaskCascadeMeta.x + 0.5), OCEAN_REFLECTED_CAUSTICS_LOCALFFT_MASK_LODS);
    if (lodCount <= 0)
        return vec4(0.0);

    float res = float(textureSize(uOceanLocalFFTMask0, 0).x);
    for (int lod = 0; lod < OCEAN_REFLECTED_CAUSTICS_LOCALFFT_MASK_LODS; ++lod)
    {
        if (lod >= lodCount)
            break;
        vec4 layer = uLocalFFTMaskCascadeOriginScale[lod];
        vec2 uv = OceanCascadeUV(worldXZ, layer.xy, layer.z, res);
        if (all(greaterThanEqual(uv, vec2(0.04))) && all(lessThanEqual(uv, vec2(0.96))))
            return clamp(SampleLocalFFTMaskTex(page, vec3(uv, float(lod))), 0.0, 1.0);
    }

    vec4 coarse = uLocalFFTMaskCascadeOriginScale[lodCount - 1];
    vec2 uv = OceanCascadeUV(worldXZ, coarse.xy, coarse.z, res);
    return clamp(SampleLocalFFTMaskTex(
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
        h += textureLod(displacementTex, vec3(worldXZ / worldSize, float(c)), 0.0).y * fade;
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
        vec3 uvw = vec3(worldXZ / worldSize, float(c));
        float localH = 0.0;
        if (stream == 0u)
            localH = textureLod(uOceanLocalDisplacement0, uvw, 0.0).y;
        else if (stream == 1u)
            localH = textureLod(uOceanLocalDisplacement1, uvw, 0.0).y;
        else if (stream == 2u)
            localH = textureLod(uOceanLocalDisplacement2, uvw, 0.0).y;
        else if (stream == 3u)
            localH = textureLod(uOceanLocalDisplacement3, uvw, 0.0).y;
        else if (stream == 4u)
            localH = textureLod(uOceanLocalDisplacement4, uvw, 0.0).y;
        else if (stream == 5u)
            localH = textureLod(uOceanLocalDisplacement5, uvw, 0.0).y;
        else if (stream == 6u)
            localH = textureLod(uOceanLocalDisplacement6, uvw, 0.0).y;
        else
            localH = textureLod(uOceanLocalDisplacement7, uvw, 0.0).y;
        h += localH * fade;
    }
    return h;
}

float OceanHeightAt(vec2 worldXZ, float viewDist)
{
    if (uFFTParams.x < 0.5 || uFFTParams.y < 0.5)
        return GerstnerHeight(worldXZ);

    float h = OceanFFTHeightAt(uOceanDisplacement, worldXZ, viewDist);
    uint localMask = LocalFFTMaskBits();
    if (localMask != 0u)
    {
        vec4 localWeights0 = SampleLocalFFTMask(worldXZ, 0u);
        if ((localMask & 1u) != 0u && localWeights0.x > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 0u), localWeights0.x);
        if ((localMask & 2u) != 0u && localWeights0.y > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 1u), localWeights0.y);
        if ((localMask & 4u) != 0u && localWeights0.z > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 2u), localWeights0.z);
        if ((localMask & 8u) != 0u && localWeights0.w > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 3u), localWeights0.w);

        vec4 localWeights1 = SampleLocalFFTMask(worldXZ, 1u);
        if ((localMask & 16u) != 0u && localWeights1.x > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 4u), localWeights1.x);
        if ((localMask & 32u) != 0u && localWeights1.y > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 5u), localWeights1.y);
        if ((localMask & 64u) != 0u && localWeights1.z > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 6u), localWeights1.z);
        if ((localMask & 128u) != 0u && localWeights1.w > 1.0e-4)
            h = mix(h, OceanLocalFFTHeightAt(worldXZ, viewDist, 7u), localWeights1.w);
    }
    h *= uFFTParams.z;
    h = clamp(h, -uFFTParams.w, uFFTParams.w);
    return uOcean.x + h;
}

vec3 OceanNormalAt(vec2 worldXZ, float viewDist)
{
    float e = 0.35;
    float hL = OceanHeightAt(worldXZ - vec2(e, 0.0), viewDist);
    float hR = OceanHeightAt(worldXZ + vec2(e, 0.0), viewDist);
    float hD = OceanHeightAt(worldXZ - vec2(0.0, e), viewDist);
    float hU = OceanHeightAt(worldXZ + vec2(0.0, e), viewDist);
    return normalize(vec3(hL - hR, 2.0 * e, hD - hU));
}

void main()
{
    vec3 original = texture(uScene, vUV).rgb;
    vec3 result = original;
    float rawDepth = texture(uDepth, vUV).r;

    // Reflected caustics above the water surface: screen-space projection onto
    // real scene geometry within a configurable height band above sea level.
    // The derivatives are taken here, in uniform control flow, before any branch.
    vec3 worldPos = WorldFromDepth(vUV, rawDepth);
    vec3 dpdx = dFdx(worldPos);
    vec3 dpdy = dFdy(worldPos);

    // A receiver is lit only between 0.08 m and the band height above the
    // displaced surface, which stays within uOcean.z of sea level. A pixel outside
    // that band keeps the scene and skips the ten surface-height probes below.
    float heightAboveSeaLevel = worldPos.y - uOcean.x;
    bool inBand = heightAboveSeaLevel > 0.08 - uOcean.z
               && heightAboveSeaLevel < max(uReflectedCaustics.y, 1.0e-3) + uOcean.z;
    if (uOcean.y < 0.5 || rawDepth <= 1.0e-6 || !inBand)
    {
        outColor = vec4(original, 1.0);
        return;
    }

    vec3 screenNormal = normalize(cross(dpdx, dpdy));
    float sideMask = 1.0 - smoothstep(0.28, 0.72, abs(screenNormal.y));
    float viewDist = length(worldPos - uCameraPos.xyz);
    vec3 sunDir = normalize(uSunDirection.xyz);
    float surfaceH0 = OceanHeightAt(worldPos.xz, viewDist);
    float heightAbove0 = max(worldPos.y - surfaceH0, 0.0);
    vec3 surfaceNormal0 = OceanNormalAt(worldPos.xz, viewDist);
    vec3 reflectedDir0 = reflect(-sunDir, surfaceNormal0);
    float reflectedY0 = max(reflectedDir0.y, uOcean.w);
    vec2 surfaceXZ = worldPos.xz - reflectedDir0.xz * heightAbove0 / reflectedY0;
    float surfaceH = OceanHeightAt(surfaceXZ, viewDist);
    vec3 surfaceNormal = OceanNormalAt(surfaceXZ, viewDist);
    vec3 reflectedDir = reflect(-sunDir, surfaceNormal);
    float reflectedUp = smoothstep(0.02, 0.18, reflectedDir.y);
    float heightAbove = worldPos.y - surfaceH;
    float aboveSurfaceMask = smoothstep(0.08, 0.24, heightAbove);
    float maxHeight = max(uReflectedCaustics.y, 0.0);
    float heightMask = (1.0 - smoothstep(0.0, max(maxHeight, 1.0e-3), heightAbove))
                     * exp(-max(heightAbove, 0.0) * max(uReflectedCaustics.z, 0.01))
                     * aboveSurfaceMask;
    float sunAbove = smoothstep(0.02, 0.16, normalize(uSunDirection.xyz).y);
    if (heightMask > 0.001 && sideMask > 0.001 && reflectedUp > 0.001 && sunAbove > 0.001)
    {
        vec2 basePos = surfaceXZ;
        float invScale = 1.0 / max(uCaustics.x, 1.0e-3);
        float t = uTime.x;
        float a = textureLod(uOceanCaustics,
                             basePos * invScale + vec2(0.044 * t + 17.16, -0.169 * t), 0.0).b;
        float b = textureLod(uOceanCaustics,
                             1.37 * basePos * invScale + vec2(0.248 * t, 0.117 * t), 0.0).b;
        float caustic = (a * b - uCaustics.y) * uCaustics.z;
        float weight = heightMask * sideMask * reflectedUp * sunAbove * max(uReflectedCaustics.x, 0.0);
        result *= max(vec3(0.0), vec3(1.0 + caustic * weight));
    }

    outColor = vec4(result, 1.0);
}
