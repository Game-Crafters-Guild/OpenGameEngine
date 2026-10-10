#ifndef GE_VOLUMETRIC_FOG_COMMON_GLSL
#define GE_VOLUMETRIC_FOG_COMMON_GLSL

layout(set = 0, binding = 2, std140) uniform FogParams
{
    mat4 uInvViewProj;
    mat4 uView;
    vec4 uCameraPos;
    vec4 uSunDirection;
    vec4 uSunColor;
    vec4 uAmbientColor; // rgb=flat-ambient fallback tint, w=IBL intensity scale
    vec4 uAlbedoDensity;
    vec4 uEmissionAnisotropy;
    vec4 uHeightParams; // x=baseHeight, y=heightFalloff, z=localLightHistoryPressure, w=isGlobal
    vec4 uLocalVolumeCenterShape; // xyz=centerWS, w=shape (0 box, 1 sphere, 2 capsule, 3 cylinder)
    vec4 uLocalVolumeAxisXHalfExtent; // xyz=axisX, w=halfExtentX
    vec4 uLocalVolumeAxisYHalfExtent; // xyz=axisY, w=halfExtentY
    vec4 uLocalVolumeAxisZHalfExtent; // xyz=axisZ, w=halfExtentZ
    vec4 uLocalVolumeParams; // x=blendDistance, y=valid, z=skyFade (far fog->sky feather)
    vec4 uNoiseParams;
    vec4 uNoiseVelocityTime;
    vec4 uNoiseChannelWeights;
    vec4 uDensityParams; // x=global noise threshold, y=threshold softness, z=orthographic, w=iblEnabled
    vec4 uVolumeParams;
    vec4 uGridParams;
    vec4 uJitterParams; // x=frame, y=historyValid, z=renderWidth, w=renderHeight
    vec4 uFogExtraParams; // x=compositeDepthBias, y=shadowBias, z=reserved, w=emissionGridActive
    mat4 uPrevViewProj;
    vec4 uPrevCameraPos;
} fog;

#ifdef GE_VOLUMETRIC_FOG_BIND_JITTER_ATLAS
layout(set = 0, binding = 11) uniform sampler3D uFogJitterAtlas;
#endif

#ifdef GE_VOLUMETRIC_FOG_BIND_LOCAL_VOLUMES
struct FogLocalVolumePacked
{
    vec4 centerShape;
    vec4 axisXHalfExtent;
    vec4 axisYHalfExtent;
    vec4 axisZHalfExtent;
    vec4 params0; // x=blend, y=weight, z=density, w=mode
    vec4 albedo;
    vec4 emission;
    vec4 gradientLow;
    vec4 gradientHigh;
    vec4 params1; // x=threshold, y=softness, z=gradientMode, w=gradientStrength
};

layout(set = 0, binding = 12, std430) readonly buffer FogLocalVolumeBuffer
{
    uvec4 uFogLocalVolumeHeader;
    FogLocalVolumePacked uFogLocalVolumes[];
};
#endif

#ifdef GE_VOLUMETRIC_FOG_BIND_DENSITY_NOISE
layout(set = 0, binding = 13) uniform sampler3D uFogDensityNoiseAtlas;
#endif

vec2 FogUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

vec2 FogNdcToUv(vec2 ndc)
{
    return vec2(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
}

float FogSliceDistance(float slice01)
{
    return fog.uVolumeParams.x * pow(clamp(slice01, 0.0, 1.0), max(fog.uVolumeParams.y, 0.05));
}

float FogDistanceToSlice01(float distanceToCamera)
{
    return pow(clamp(distanceToCamera / max(fog.uVolumeParams.x, 0.001), 0.0, 1.0), 1.0 / max(fog.uVolumeParams.y, 0.05));
}

vec3 FogWorldRayFromUv(vec2 uv)
{
    vec4 nearPoint = fog.uInvViewProj * vec4(FogUvToNdc(uv), 1.0, 1.0);
    vec4 farPoint = fog.uInvViewProj * vec4(FogUvToNdc(uv), 0.0, 1.0);
    nearPoint.xyz /= max(nearPoint.w, 1.0e-6);
    farPoint.xyz /= max(farPoint.w, 1.0e-6);
    if (fog.uDensityParams.z > 0.5)
        return normalize(farPoint.xyz - nearPoint.xyz);
    return normalize(farPoint.xyz - fog.uCameraPos.xyz);
}

vec3 FogRayOriginFromUv(vec2 uv)
{
    if (fog.uDensityParams.z <= 0.5)
        return fog.uCameraPos.xyz;

    vec4 nearPoint = fog.uInvViewProj * vec4(FogUvToNdc(uv), 1.0, 1.0);
    return nearPoint.xyz / max(nearPoint.w, 1.0e-6);
}

vec3 FogWorldFromUvDistance(vec2 uv, float distanceToCamera)
{
    return FogRayOriginFromUv(uv) + FogWorldRayFromUv(uv) * distanceToCamera;
}

vec3 FogWorldFromDepth(vec2 uv, float rawDepth)
{
    vec4 p = fog.uInvViewProj * vec4(FogUvToNdc(uv), rawDepth, 1.0);
    return p.xyz / max(p.w, 1.0e-6);
}

float FogViewDistanceFromWorld(vec2 uv, vec3 worldPos)
{
    if (fog.uDensityParams.z > 0.5)
        return max(dot(worldPos - FogRayOriginFromUv(uv), FogWorldRayFromUv(uv)), 0.0);
    return length(worldPos - fog.uCameraPos.xyz);
}

float FogHash(vec3 p)
{
    p = fract(p * 0.3183099 + vec3(0.11, 0.23, 0.37));
    p += dot(p, p.yzx + 19.19);
    return fract((p.x + p.y) * p.z);
}

uint FogPcg(uint v)
{
    v = v * 747796405u + 2891336453u;
    uint word = ((v >> ((v >> 28u) + 4u)) ^ v) * 277803737u;
    return (word >> 22u) ^ word;
}

float FogStbnStyleScalar(ivec3 id)
{
    uint frame = uint(max(fog.uJitterParams.x, 0.0));
#ifdef GE_VOLUMETRIC_FOG_BIND_JITTER_ATLAS
    vec3 atlasCoord = (vec3(ivec3(id) & ivec3(31)) + vec3(0.5)) * (1.0 / 32.0);
    float base = texture(uFogJitterAtlas, atlasCoord).r;
#else
    uint spatial = FogPcg(uint(id.x) * 1973u ^ uint(id.y) * 9277u ^ uint(id.z) * 26699u ^ 0x9E3779B9u);
    float base = float(spatial & 0x00FFFFFFu) * (1.0 / 16777216.0);
#endif
    float temporal = fract(float(frame & 1023u) * 0.6180339887498948);
    uint x = uint(id.x) & 31u;
    uint y = uint(id.y) & 31u;
    uint z = uint(id.z) & 31u;
    float slicePhase = fract(float((x + y * 7u + z * 13u) & 31u) * (1.0 / 32.0));
    return fract(base + temporal + slicePhase);
}

float FogFrameJitter(ivec3 id)
{
    return (FogStbnStyleScalar(id) - 0.5) * fog.uVolumeParams.w;
}

float FogLocalVolumeOutsideDistance(vec3 worldPos)
{
    vec3 delta = worldPos - fog.uLocalVolumeCenterShape.xyz;
    vec3 localPos = vec3(
        dot(delta, fog.uLocalVolumeAxisXHalfExtent.xyz),
        dot(delta, fog.uLocalVolumeAxisYHalfExtent.xyz),
        dot(delta, fog.uLocalVolumeAxisZHalfExtent.xyz));
    vec3 halfExtents = max(vec3(
        fog.uLocalVolumeAxisXHalfExtent.w,
        fog.uLocalVolumeAxisYHalfExtent.w,
        fog.uLocalVolumeAxisZHalfExtent.w), vec3(0.001));

    int shape = int(fog.uLocalVolumeCenterShape.w + 0.5);
    if (shape == 1)
    {
        vec3 k1v = localPos / halfExtents;
        vec3 k2v = localPos / (halfExtents * halfExtents);
        float k1 = length(k1v);
        float k2 = length(k2v);
        return max(k1 > 0.0 && k2 > 0.0 ? k1 * (k1 - 1.0) / k2 : 0.0, 0.0);
    }
    if (shape == 2 || shape == 3)
    {
        float radial = max(halfExtents.x, halfExtents.z);
        float axisCore = max(halfExtents.y - radial, 0.0);
        float qy = abs(localPos.y) - axisCore;
        float qLen = length(localPos.xz);
        if (shape == 2)
        {
            float dx = qLen;
            float dy = max(qy, 0.0);
            return max(sqrt(dx * dx + dy * dy) - radial, 0.0);
        }

        float dx = qLen - radial;
        float dy = max(qy, 0.0);
        float dist = max(dx, 0.0);
        if (dy > 0.0)
            dist = sqrt(max(dx, 0.0) * max(dx, 0.0) + dy * dy);
        return max(dist, 0.0);
    }

    vec3 outside = max(abs(localPos) - halfExtents, vec3(0.0));
    return length(outside);
}

#ifdef GE_VOLUMETRIC_FOG_BIND_LOCAL_VOLUMES
float FogLocalVolumeOutsideDistancePacked(vec3 worldPos, vec4 centerShape, vec4 axisXHalfExtent, vec4 axisYHalfExtent, vec4 axisZHalfExtent)
{
    vec3 delta = worldPos - centerShape.xyz;
    vec3 localPos = vec3(
        dot(delta, axisXHalfExtent.xyz),
        dot(delta, axisYHalfExtent.xyz),
        dot(delta, axisZHalfExtent.xyz));
    vec3 halfExtents = max(vec3(axisXHalfExtent.w, axisYHalfExtent.w, axisZHalfExtent.w), vec3(0.001));

    int shape = int(centerShape.w + 0.5);
    if (shape == 1)
    {
        vec3 k1v = localPos / halfExtents;
        vec3 k2v = localPos / (halfExtents * halfExtents);
        float k1 = length(k1v);
        float k2 = length(k2v);
        return max(k1 > 0.0 && k2 > 0.0 ? k1 * (k1 - 1.0) / k2 : 0.0, 0.0);
    }
    if (shape == 2 || shape == 3)
    {
        float radial = max(halfExtents.x, halfExtents.z);
        float axisCore = max(halfExtents.y - radial, 0.0);
        float qy = abs(localPos.y) - axisCore;
        float qLen = length(localPos.xz);
        if (shape == 2)
        {
            float dx = qLen;
            float dy = max(qy, 0.0);
            return max(sqrt(dx * dx + dy * dy) - radial, 0.0);
        }

        float dx = qLen - radial;
        float dy = max(qy, 0.0);
        float dist = max(dx, 0.0);
        if (dy > 0.0)
            dist = sqrt(max(dx, 0.0) * max(dx, 0.0) + dy * dy);
        return max(dist, 0.0);
    }

    vec3 outside = max(abs(localPos) - halfExtents, vec3(0.0));
    return length(outside);
}

float FogLocalVolumeWeightPacked(vec3 worldPos, FogLocalVolumePacked volume)
{
    float outsideDistance = FogLocalVolumeOutsideDistancePacked(
        worldPos,
        volume.centerShape,
        volume.axisXHalfExtent,
        volume.axisYHalfExtent,
        volume.axisZHalfExtent);
    if (outsideDistance <= 0.0)
        return clamp(volume.params0.y, 0.0, 64.0);

    float blend = max(volume.params0.x, 0.0);
    if (blend <= 0.0 || outsideDistance >= blend)
        return 0.0;

    float t = 1.0 - outsideDistance / blend;
    return t * t * (3.0 - 2.0 * t) * clamp(volume.params0.y, 0.0, 64.0);
}

vec3 FogLocalPositionPacked(vec3 worldPos, FogLocalVolumePacked volume)
{
    vec3 delta = worldPos - volume.centerShape.xyz;
    vec3 halfExtents = max(vec3(volume.axisXHalfExtent.w, volume.axisYHalfExtent.w, volume.axisZHalfExtent.w), vec3(0.001));
    return vec3(
        dot(delta, volume.axisXHalfExtent.xyz),
        dot(delta, volume.axisYHalfExtent.xyz),
        dot(delta, volume.axisZHalfExtent.xyz)) / halfExtents;
}

vec3 FogLocalPositionWorldPacked(vec3 worldPos, FogLocalVolumePacked volume)
{
    vec3 delta = worldPos - volume.centerShape.xyz;
    return vec3(
        dot(delta, volume.axisXHalfExtent.xyz),
        dot(delta, volume.axisYHalfExtent.xyz),
        dot(delta, volume.axisZHalfExtent.xyz));
}

float FogLocalGradientFactor(vec3 worldPos, vec2 uv, vec3 sunDir, FogLocalVolumePacked volume)
{
    int mode = int(volume.params1.z + 0.5);
    if (mode == 1) return clamp(FogLocalPositionPacked(worldPos, volume).x * 0.5 + 0.5, 0.0, 1.0);
    if (mode == 2) return clamp(FogLocalPositionPacked(worldPos, volume).y * 0.5 + 0.5, 0.0, 1.0);
    if (mode == 3) return clamp(FogLocalPositionPacked(worldPos, volume).z * 0.5 + 0.5, 0.0, 1.0);
    if (mode == 4) return clamp(1.0 - uv.y, 0.0, 1.0);
    if (mode == 5) return clamp(dot(normalize(worldPos - volume.centerShape.xyz), normalize(sunDir)) * 0.5 + 0.5, 0.0, 1.0);
    return 0.5;
}
#endif

float FogLocalVolumeWeight(vec3 worldPos)
{
    if (fog.uHeightParams.w > 0.5)
        return 1.0;
    if (fog.uLocalVolumeParams.y < 0.5)
        return 0.0;

    float outsideDistance = FogLocalVolumeOutsideDistance(worldPos);
    if (outsideDistance <= 0.0)
        return 1.0;

    float blend = max(fog.uLocalVolumeParams.x, 0.0);
    if (blend <= 0.0 || outsideDistance >= blend)
        return 0.0;

    float t = 1.0 - outsideDistance / blend;
    return t * t * (3.0 - 2.0 * t);
}

float FogSliceCoord(int z, float jitter)
{
    return clamp((float(z) + 0.5 + jitter) / max(fog.uGridParams.z, 1.0), 0.0, 1.0);
}

float FogValueNoise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = FogHash(i + vec3(0.0, 0.0, 0.0));
    float n100 = FogHash(i + vec3(1.0, 0.0, 0.0));
    float n010 = FogHash(i + vec3(0.0, 1.0, 0.0));
    float n110 = FogHash(i + vec3(1.0, 1.0, 0.0));
    float n001 = FogHash(i + vec3(0.0, 0.0, 1.0));
    float n101 = FogHash(i + vec3(1.0, 0.0, 1.0));
    float n011 = FogHash(i + vec3(0.0, 1.0, 1.0));
    float n111 = FogHash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    return mix(mix(nx00, nx10, f.y), mix(nx01, nx11, f.y), f.z);
}

float FogFbm(vec3 p)
{
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i)
    {
        sum += amp * FogValueNoise(p);
        p = p * 2.03 + vec3(17.0, 31.0, 47.0);
        amp *= 0.5;
    }
    return sum;
}

float FogDensityNoise(vec3 p)
{
#ifdef GE_VOLUMETRIC_FOG_BIND_DENSITY_NOISE
    vec4 channels = texture(uFogDensityNoiseAtlas, fract(p)).rgba;
    float weightSum = max(dot(fog.uNoiseChannelWeights, vec4(1.0)), 0.0001);
    return dot(channels, fog.uNoiseChannelWeights) / weightSum;
#else
    return FogFbm(p * 64.0);
#endif
}

float FogApplyDensityThreshold(float n, float threshold, float softness)
{
    threshold = clamp(threshold, 0.0, 1.0);
    if (threshold <= 0.0)
        return n;
    return smoothstep(threshold, min(threshold + max(softness, 0.0001), 1.0), n);
}

float FogHenyeyGreenstein(float cosTheta, float g)
{
    float gg = g * g;
    float denom = max(1.0 + gg - 2.0 * g * cosTheta, 1.0e-4);
    return (1.0 - gg) / (12.5663706 * denom * sqrt(denom));
}

#endif
