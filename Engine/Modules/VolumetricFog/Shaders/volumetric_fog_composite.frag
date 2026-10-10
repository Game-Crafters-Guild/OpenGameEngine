#version 450

#include "Includes/compat_profile.glsl"

#include "volumetric_fog_common.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler3D uIntegrated;
layout(set = 0, binding = 1) uniform sampler2D uDepth;
// Same engine-shared irradiance cube the lighting pass uses, so the analytic
// far-fog in-scatter color matches the near (froxel) fog. Sampled only on the
// far-extension path, gated by fog.uDensityParams.w (iblEnabled).
layout(set = 0, binding = 3) uniform samplerCube uFogIrradianceCube;

// Antiderivative of exp(-max(s,0)/H) in s, made continuous at s=0 (G(0)=0): below
// the fog base density is constant (slope 1), above it decays exponentially.
float FogHeightG(float s, float H)
{
    return s < 0.0 ? s : H * (1.0 - exp(-s / H));
}

// Analytic exponential height-fog continuation past the froxel grid's far plane.
// The grid covers [0, maxDistance]; geometry/sky beyond it would otherwise get no
// fog. The optical depth of the SAME density model the media pass uses
// (density0 * exp(-(h-base)/falloff)) has a closed form along the ray, so this is
// evaluated analytically — NOT ray-marched — which avoids step banding entirely.
// Anchored to the grid's last-slice (scatter, transmittance): a zero-length
// segment at the boundary reproduces the grid value, so the join is seamless.
vec4 FogExtendBeyondGrid(vec4 gridFog, vec2 uv, float sceneDistance)
{
    const float kFarRange = 40000.0; // cap for sky / degenerate far depths
    // Feather width of the fog->sky join (VolumetricFogEffect.SkyFade); min keeps
    // the smoothstep well-formed so 0 reads as a near-hard horizon edge.
    float skyFadeBand = max(fog.uLocalVolumeParams.z, 0.02);

    float maxDist = fog.uVolumeParams.x;
    float farEnd = min(sceneDistance, maxDist + kFarRange);
    float seg = farEnd - maxDist;
    if (seg <= 0.0)
        return gridFog;

    vec3 ro = FogRayOriginFromUv(uv);
    vec3 rd = FogWorldRayFromUv(uv);
    float base = fog.uHeightParams.x;
    float H = max(fog.uHeightParams.y, 0.01);
    float density0 = max(fog.uAlbedoDensity.w, 0.0);

    // Closed-form optical depth over [maxDist, farEnd]: density0 * segLength *
    // (average of exp(-max(h-base,0)/H) along the segment), the average being the
    // analytic integral G(yEnd)-G(yStart) over the height delta.
    float ya = (ro + rd * maxDist).y - base;
    float yb = (ro + rd * farEnd).y - base;
    float dy = yb - ya;
    float heightIntegral = abs(dy) < 1.0e-4
        ? exp(-max(ya, 0.0) / H)
        : (FogHeightG(yb, H) - FogHeightG(ya, H)) / dy;
    float Tfar = exp(-density0 * seg * max(heightIntegral, 0.0));

    // Representative in-scatter color (sun + GI ambient), constant across the
    // segment -> smooth on screen. Same model as the lighting pass so near and far
    // fog read identically.
    vec3 sunDir = normalize(fog.uSunDirection.xyz);
    vec3 direct = fog.uSunColor.rgb * FogHenyeyGreenstein(dot(rd, sunDir), fog.uEmissionAnisotropy.w);
    vec3 ambient;
    if (fog.uDensityParams.w > 0.5)
    {
        // GE_TAP_LOD0: WGSL's uniformity analysis cannot prove this branch
        // uniform (naga routes the UBO read through a private) and allows
        // implicit-derivative sampling only in uniform flow, so the compat arm
        // needs the explicit level. The irradiance cube is mip-less, so level 0
        // is the same fetch either way.
        vec3 skyIrr = GE_TAP_LOD0(uFogIrradianceCube, vec3(0.0, 1.0, 0.0)).rgb;
        vec3 groundIrr = GE_TAP_LOD0(uFogIrradianceCube, vec3(0.0, -1.0, 0.0)).rgb;
        ambient = mix(groundIrr, skyIrr, clamp(0.5 * (ya + yb) / H, 0.0, 1.0)) * fog.uAmbientColor.rgb;
    }
    else
    {
        ambient = fog.uAmbientColor.rgb * 0.18;
    }
    vec3 fogColor = fog.uAlbedoDensity.rgb * (direct + ambient) + fog.uEmissionAnisotropy.rgb;

    // Full fog beyond the grid: the near grid band plus the analytic far term.
    vec3 outRgb = gridFog.rgb + fogColor * ((1.0 - Tfar) * gridFog.a);
    float outTrans = gridFog.a * Tfar;

    // Feather the WHOLE fog->sky join above the horizon. Fading both the near grid
    // band and the far term (not just the far term) removes the residual horizon
    // band, and a quintic smootherstep ease gives a gradient with no perceptible
    // edge. Rays at/below the horizon (rd.y <= 0) keep full fog, so ground and
    // geometry are untouched; only the sky clears toward the zenith.
    float u = clamp(max(rd.y, 0.0) / skyFadeBand, 0.0, 1.0);
    float skyKeep = 1.0 - u * u * u * (u * (u * 6.0 - 15.0) + 10.0);
    return vec4(outRgb * skyKeep, mix(1.0, outTrans, skyKeep));
}

float FogSceneDistance(vec2 uv, ivec2 depthSize)
{
    ivec2 px = ivec2(clamp(uv * vec2(depthSize), vec2(0.0), vec2(depthSize - ivec2(1))));
    float rawDepth = texelFetch(uDepth, px, 0).r;
    vec3 worldPos = FogWorldFromDepth(uv, rawDepth);
    return max(FogViewDistanceFromWorld(uv, worldPos) + fog.uFogExtraParams.x, 0.0);
}

vec4 FogBilateralIntegrated(vec2 uv, float centerSlice01, float centerDistance, ivec2 depthSize)
{
    vec2 fogTexel = 1.0 / max(fog.uGridParams.xy, vec2(1.0));
    vec2 offsets[5] = vec2[](
        vec2(0.0, 0.0),
        vec2(1.0, 0.0),
        vec2(-1.0, 0.0),
        vec2(0.0, 1.0),
        vec2(0.0, -1.0));

    vec4 sum = vec4(0.0);
    float weightSum = 0.0;
    for (int i = 0; i < 5; ++i)
    {
        vec2 sampleUv = clamp(uv + offsets[i] * fogTexel, vec2(0.0), vec2(1.0));
        float sampleDistance = FogSceneDistance(sampleUv, depthSize);
        float sampleSlice01 = FogDistanceToSlice01(sampleDistance);
        float depthWeight = exp(-abs(sampleSlice01 - centerSlice01) * max(fog.uGridParams.z * 0.65, 1.0));
        float screenWeight = (i == 0) ? 1.0 : 0.55;
        float distanceRatio = abs(sampleDistance - centerDistance) / max(centerDistance, 1.0);
        float edgeWeight = exp(-distanceRatio * 24.0);
        float w = screenWeight * max(depthWeight, edgeWeight * 0.35);
        sum += texture(uIntegrated, vec3(sampleUv, clamp(sampleSlice01, 0.0, 1.0))) * w;
        weightSum += w;
    }
    return sum / max(weightSum, 1.0e-4);
}

void main()
{
    ivec2 depthSize = textureSize(uDepth, 0);
    vec2 uv = clamp(vUV, vec2(0.0), vec2(1.0));
    float distanceToCamera = FogSceneDistance(uv, depthSize);
    float slice01 = FogDistanceToSlice01(distanceToCamera);

    vec4 fogSample = FogBilateralIntegrated(uv, clamp(slice01, 0.0, 1.0), distanceToCamera, depthSize);

    // Global fog only: extend analytically past the grid so distant geometry/sky
    // (beyond maxDistance, e.g. when the camera is zoomed out) stays fogged.
    if (fog.uHeightParams.w > 0.5 && distanceToCamera > fog.uVolumeParams.x)
        fogSample = FogExtendBeyondGrid(fogSample, uv, distanceToCamera);

    outColor = vec4(fogSample.rgb, fogSample.a);
}
