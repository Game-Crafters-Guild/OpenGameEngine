// Clustered Forward+ light traversal include.
//
// Provides GE_AccumulateClusteredLighting() which iterates all lights
// affecting the current fragment's cluster and accumulates their contribution
// using the active lighting model BRDF.
//
// Required bindings (set 0, must match ForwardPlus.rendergraph resource layout):
//   binding 0: LightIndexBuffer (SSBO, uint[])
//   binding 1: ClusterBuffer (SSBO, uvec4[])
//   binding 2: ClusterParams (UBO, uvec4 params0)
//   binding 4: LightBuffer (SSBO, header + LightPacked[])
//   binding 7: ViewParams (UBO, mat4 invProj, mat4 view, vec4 nearFar, vec4 cameraPosWS)
//
// Binding 5 is reserved for the vertex shader CameraUBO (uV, uP, uVP, uCameraPos).
// ViewParams is at binding 7 to avoid the conflict.
//
// Optional shadow bindings (when HAS_SHADOWS is defined):
//   binding 8: ShadowData (UBO, cascade VPs + splits + params)
//   binding 9: ge_shadowMapArray (sampler2DArrayShadow)
//   binding 14: AreaShadowData (UBO, single area-light local transform + params)
//   binding 15: ge_areaShadowMap (sampler2DShadow)
//   binding 16: ge_areaShadowMapRaw (sampler2D; absent under GE_COMPAT_PROFILE)
//   binding 23: SpotShadowData (UBO, single spot-light VP + params)
//   binding 24: ge_spotShadowMap (sampler2DShadow)
//   binding 25: PointShadowData (SSBO, PointShadowSlot[] atlas slots — M1)
//   binding 26: ge_pointShadowMap (sampler2DArrayShadow, tiered atlas)

#ifndef GE_CLUSTERED_LIGHTING_GLSL
#define GE_CLUSTERED_LIGHTING_GLSL

#include "compat_profile.glsl"

#include "surface_io.glsl"   // SurfaceOutput — surface props threaded to the BRDF
#include "surface_light_response.glsl" // GE_EvaluateSurfaceLight(SurfaceOutput, V, L) + GE_PI

#ifdef HAS_SHADOWS
#include "shadow_sampling.glsl"
layout(set = 0, binding = 14, std140) uniform AreaShadowData
{
    mat4 ge_areaShadowVP;
    vec4 ge_areaShadowParams;  // x=enabled, y=cluster-light index, z=depthBias, w=normalBias
    vec4 ge_areaShadowParams2; // x=resolution, y=range, z=light size, w=unused
};
layout(set = 0, binding = 15) uniform sampler2DShadow ge_areaShadowMap;
#if !defined(GE_COMPAT_PROFILE)
// Raw (non-comparison) view of the area shadow map, read by the PCSS blocker
// search. Omitted under the compat profile: WebGPU only lets a depth texture be
// bound to a depth or comparison binding, and GLSL cannot declare a depth
// texture that is sampled without comparison, so the blocker search is replaced
// by the light-size penumbra estimate below.
layout(set = 0, binding = 16) uniform sampler2D ge_areaShadowMapRaw;
#endif
layout(set = 0, binding = 23, std140) uniform SpotShadowData
{
    mat4 ge_spotShadowVP;
    vec4 ge_spotShadowParams;  // x=enabled, y=cluster-light index, z=depthBias, w=normalBias
    vec4 ge_spotShadowParams2; // x=resolution, y=near plane, z=far plane, w=unused
};
layout(set = 0, binding = 24) uniform sampler2DShadow ge_spotShadowMap;
// M1: one entry per atlas slot (mirrors RenderServices.h PointShadowSlotGPU, 416 B).
struct PointShadowSlot
{
    mat4 faceVP[6];
    vec4 params;  // x=enabled, y=baseLayer (slot*6), z=depthBias, w=normalBias
    vec4 params2; // x=tileResolution, y=near, z=far, w=tileScale (tileRes/atlasRes)
};
layout(set = 0, binding = 25, std430) readonly buffer PointShadowData
{
    PointShadowSlot ge_pointShadowSlots[];
};
layout(set = 0, binding = 26) uniform sampler2DArrayShadow ge_pointShadowMap;
#endif

// Light data matching the CPU-side ExtractedLight / LightUploadNode layout.
// Shares the field list with the standalone Forward+ shaders' LightPacked.
struct GE_LightPacked
{
#include "light_packed_fields.glsl"
};

// Cluster/light index buffers. Schema as of Phase D step 4:
//   ClusterBuffer:
//     ge_clusterHeader @ [0]            = (clustersX, clustersY, depthSlices, maxLightsPerCluster)
//     ge_clusters[3DclusterId]          = uvec2(firstIndex, count)
//     where 3DclusterId = tx + ty*cX + sli*cX*cY
//     and firstIndex    = 3DclusterId * maxLightsPerCluster
//   LightIndexBuffer:
//     ge_lightIndices[firstIndex + i]   = light index (uint32)
layout(set = 0, binding = 0, std430) readonly buffer LightIndexBuffer
{
    uint ge_lightIndices[];
};

layout(set = 0, binding = 1, std430) readonly buffer ClusterBuffer
{
    uvec4 ge_clusterHeader;
    uvec2 ge_clusters[];
};

layout(set = 0, binding = 2, std140) uniform ClusterParams
{
    uvec4 ge_clusterParams0; // tileX, tileY, depthSlices, maxLightsPerCluster
};

layout(set = 0, binding = 4, std430) readonly buffer LightBuffer
{
    uvec4 ge_lightHeader;
    GE_LightPacked ge_lights[];
};

#include "view_params.glsl"

float GE_SmoothRangeAttenuation(float dist, float range)
{
    float distNorm = dist / max(range, 0.001);
    float attenuation = max(1.0 - distNorm * distNorm, 0.0);
    return attenuation * attenuation;
}

float GE_LocalLightAttenuation(float dist, float range, float decay, float falloffMode)
{
    uint mode = uint(clamp(floor(falloffMode + 0.5), 0.0, 3.0));
    if (mode == 1u) // Linear
        return max(1.0 - dist / max(range, 0.001), 0.0);

    float rangeAtten = GE_SmoothRangeAttenuation(dist, range);
    if (mode == 2u) // Smooth Range
        return rangeAtten;

    float safeDist = max(dist, 1.0);
    if (mode == 3u) // Custom
    {
        if (decay > 0.01)
            rangeAtten /= pow(safeDist, decay * 0.5);
        return rangeAtten;
    }

    // Physical / Inverse Square
    return rangeAtten / (safeDist * safeDist);
}

float GE_AreaLightExtent(GE_LightPacked lp)
{
    uint shape = lp.meta.z;
    float w = max(lp.colorAreaWidth.w, 0.001);
    float h = max(lp.areaParams.x, 0.001);
    float r = max(lp.areaParams.y, 0.001);
    if (shape == 0u) return 0.5 * length(vec2(w, h));
    if (shape == 1u) return r;
    if (shape == 2u) return r;
    return length(vec2(r, 0.5 * h));
}

vec3 GE_SafeAreaRight(vec3 normal, vec3 authoredRight)
{
    vec3 right = authoredRight - normal * dot(authoredRight, normal);
    if (dot(right, right) < 1e-5)
    {
        vec3 fallback = abs(normal.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
        right = normalize(cross(fallback, normal));
    }
    else
    {
        right = normalize(right);
    }
    return right;
}

vec3 GE_EvaluateAreaLightSample(
    vec3 samplePosWS,
    vec3 emitterNormalWS,
    bool oneSided,
    float sourceArea,
    float sampleWeight,
    GE_LightPacked lp,
    vec3 lightColor,
    SurfaceOutput so,
    vec3 V,
    vec3 posWS)
{
    vec3 lightToFrag = posWS - samplePosWS;
    float dist = length(lightToFrag);
    float range = max(lp.posRange.w + GE_AreaLightExtent(lp), 0.001);
    if (dist > range)
        return vec3(0.0);

    vec3 lightToFragN = lightToFrag / max(dist, 1e-6);
    if (oneSided)
    {
        float facing = dot(lightToFragN, emitterNormalWS);
        if (facing <= 0.0)
            return vec3(0.0);
        lightColor *= facing;
    }

    vec3 L = -lightToFragN;
    float attenuation = GE_LocalLightAttenuation(dist, range, max(lp.areaParams.z, 0.0), lp.areaUp.w);
    vec3 brdf = GE_EvaluateSurfaceLight(so, V, L);
    return brdf * lightColor * attenuation * max(sourceArea, 0.001) * sampleWeight;
}

vec3 GE_EvaluateAreaLightFinite(
    GE_LightPacked lp,
    vec3 lightColor,
    SurfaceOutput so,
    vec3 V,
    vec3 posWS)
{
    vec3 center = lp.posRange.xyz;
    vec3 normal = normalize(lp.dirIntensity.xyz);
    vec3 right = GE_SafeAreaRight(normal, lp.areaRight.xyz);
    vec3 up = normalize(cross(normal, right));

    uint shape = lp.meta.z;
    float width = max(lp.colorAreaWidth.w, 0.001);
    float height = max(lp.areaParams.x, 0.001);
    float radius = max(lp.areaParams.y, 0.001);

    vec3 total = vec3(0.0);
    if (shape == 0u)
    {
        float hx = 0.5 * width;
        float hy = 0.5 * height;
        float q = 0.57735026919;
        float area = width * height;
        total += GE_EvaluateAreaLightSample(center + right * (-hx * q) + up * (-hy * q), normal, true, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + right * ( hx * q) + up * (-hy * q), normal, true, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + right * (-hx * q) + up * ( hy * q), normal, true, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + right * ( hx * q) + up * ( hy * q), normal, true, area, 0.25, lp, lightColor, so, V, posWS);
    }
    else if (shape == 1u)
    {
        float r = radius * 0.70710678118;
        float area = GE_PI * radius * radius;
        total += GE_EvaluateAreaLightSample(center, normal, true, area, 0.20, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + right * r, normal, true, area, 0.20, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - right * r, normal, true, area, 0.20, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + up * r, normal, true, area, 0.20, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - up * r, normal, true, area, 0.20, lp, lightColor, so, V, posWS);
    }
    else if (shape == 2u)
    {
        float area = 4.0 * GE_PI * radius * radius;
        vec3 sideA = right * radius;
        vec3 sideB = up * radius;
        vec3 sideC = normal * radius;
        total += GE_EvaluateAreaLightSample(center + sideA, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - sideA, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + sideB, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - sideB, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + sideC, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - sideC, normal, false, area, 1.0 / 6.0, lp, lightColor, so, V, posWS);
    }
    else
    {
        float halfH = 0.5 * height;
        float area = 2.0 * GE_PI * radius * max(height, 0.001);
        total += GE_EvaluateAreaLightSample(center + right * radius + up * (-halfH), normal, false, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - right * radius + up * (-halfH), normal, false, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center + right * radius + up * ( halfH), normal, false, area, 0.25, lp, lightColor, so, V, posWS);
        total += GE_EvaluateAreaLightSample(center - right * radius + up * ( halfH), normal, false, area, 0.25, lp, lightColor, so, V, posWS);
    }
    return total;
}

#ifdef HAS_SHADOWS
// ── Local-light filter radii, in UV rather than in texels ──
//
// UV is resolution-independent by construction; a texel multiple is not. Every
// radius below used to be written as N * texelSize (= N / resolution), so
// halving a shadow map doubled the blur in world units. Measured on the point
// atlas: pinning a light's tier Low(256) vs High(1024) moved its shadow edge
// width 3.3x against a control that moved <1%. Point tiers change at RUNTIME
// with screen coverage, so that was a size pop mid-scene, not just a setting.
//
// These constants equal the old texel-derived values at kLocalReferenceRes, so
// a map at that resolution renders exactly as it did and only the DEPENDENCE on
// resolution is removed.
//
// What they deliberately keep is growth with receiver distance: these are
// perspective projections, so a fixed UV radius covers more world at range,
// which reads as an edge that softens with distance. That behaviour was never
// the defect.
//
// Note what this does NOT do: spot and point still have no blocker search and
// no penumbra — this makes their fixed blur resolution-stable, it does not make
// it physical. Giving them a real penumbra is the DPCF work, not this.
const float kLocalReferenceRes    = 1024.0;
const float kLocalFilterRadiusUV  = 1.5  / kLocalReferenceRes; // spot + point PCF
const float kAreaLightRadiusMinUV = 1.0  / kLocalReferenceRes; // area light-radius floor
const float kAreaSearchMinUV      = 2.0  / kLocalReferenceRes; // area blocker-search floor
const float kAreaRadiusMinUV      = 1.25 / kLocalReferenceRes; // area penumbra floor
const float kAreaRadiusMaxUV      = 32.0 / kLocalReferenceRes; // area penumbra ceiling

float GE_AreaShadowLinearDepth(float reverseDepth)
{
    float f = max(ge_areaShadowParams2.y, 0.001);
    return f * (1.0 - clamp(reverseDepth, 0.0, 1.0));
}

vec2 GE_AreaShadowRotation(vec2 shadowUV)
{
    float resolution = max(ge_areaShadowParams2.x, 1.0);
    vec2 shadowTexel = floor(clamp(shadowUV, vec2(0.0), vec2(0.999999)) * resolution);
    float angle = fract(52.9829189 * fract(dot(shadowTexel, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

float GE_SampleAreaShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    if (ge_areaShadowParams.x < 0.5 || lp.meta.y == 0u)
        return 1.0;
    if (abs(float(lightIndex) - ge_areaShadowParams.y) > 0.5)
        return 1.0;

    float farPlane = max(ge_areaShadowParams2.y, 0.001);
    vec3 localPos = (ge_areaShadowVP * vec4(posWS, 1.0)).xyz;
    float centerDist = length(localPos);
    if (centerDist <= 1e-6 || centerDist > farPlane)
        return 1.0;

    vec3 shadowDir = localPos / centerDist;
    vec3 localNormal = normalize(mat3(ge_areaShadowVP) * normalWS);
    float normalBias = ge_areaShadowParams.w * (1.0 - abs(dot(localNormal, shadowDir)));
    vec3 biasedLocal = localPos + localNormal * normalBias;
    float receiverDist = length(biasedLocal);
    if (receiverDist <= 1e-6 || receiverDist > farPlane)
        return 1.0;

    vec3 sampleDir = biasedLocal / receiverDist;
    vec2 shadowUV = sampleDir.xy / max(1.0 + abs(sampleDir.z), 1e-6);
    shadowUV = shadowUV * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;

    float refDepth = clamp(1.0 - receiverDist / farPlane + ge_areaShadowParams.z, 0.0, 1.0);
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0)
        return 1.0;

    vec2 sc = GE_AreaShadowRotation(shadowUV);
    float lightSize = max(ge_areaShadowParams2.z, 0.001);
    float lightRadiusUV = clamp(lightSize / (3.4641016 * max(receiverDist, 0.001)),
                                kAreaLightRadiusMinUV, kAreaRadiusMaxUV);

#if defined(GE_COMPAT_PROFILE)
    // No raw depth tap is available (see ge_areaShadowMapRaw), so the penumbra
    // cannot be scaled by blocker distance. The light-size footprint alone is
    // the limit PCSS converges to when the blocker sits near the receiver
    // (quality trade: contact hardening is lost, the filter width is uniform).
    float radius = clamp(lightRadiusUV, kAreaRadiusMinUV, kAreaRadiusMaxUV);
#else
    float blockerSearchRadius = max(kAreaSearchMinUV, 0.75 * lightRadiusUV);
    float blockerDepth = 0.0;
    int blockerCount = 0;
    const int blockerTaps = 12;
    for (int i = 0; i < blockerTaps; ++i)
    {
        vec2 offset = GE_VogelDisk(i, blockerTaps, sc) * blockerSearchRadius;
        vec2 sampleUV = shadowUV + offset;
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0)
            continue;
        float sampleDepth = GE_TAP_LOD0(ge_areaShadowMapRaw, sampleUV).r;
        if (sampleDepth > refDepth)
        {
            blockerDepth += GE_AreaShadowLinearDepth(sampleDepth);
            ++blockerCount;
        }
    }

    if (blockerCount == 0)
        return 1.0;

    float avgBlockerDist = blockerDepth / float(blockerCount);
    float penumbra = ((receiverDist - avgBlockerDist) / max(avgBlockerDist, 0.001)) * lightRadiusUV;
    float radius = clamp(penumbra, kAreaRadiusMinUV, kAreaRadiusMaxUV);
#endif

    float shadow = 0.0;
    const int taps = 16;
    for (int i = 0; i < taps; ++i)
    {
        vec2 offset = GE_VogelDisk(i, taps, sc) * radius;
        shadow += GE_SHADOW_TAP(ge_areaShadowMap, vec3(shadowUV + offset, refDepth));
    }
    return shadow / float(taps);
}

vec2 GE_SpotShadowRotation(vec2 shadowUV)
{
    float resolution = max(ge_spotShadowParams2.x, 1.0);
    vec2 shadowTexel = floor(clamp(shadowUV, vec2(0.0), vec2(0.999999)) * resolution);
    float angle = fract(52.9829189 * fract(dot(shadowTexel, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

float GE_SampleSpotShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    if (ge_spotShadowParams.x < 0.5 || lp.meta.y == 0u)
        return 1.0;
    if (abs(float(lightIndex) - ge_spotShadowParams.y) > 0.5)
        return 1.0;

    vec3 lightToFrag = posWS - lp.posRange.xyz;
    float dist = length(lightToFrag);
    if (dist <= 1e-6 || dist > ge_spotShadowParams2.z)
        return 1.0;

    vec3 lightToFragN = lightToFrag / dist;
    vec3 n = normalize(normalWS);
    float normalBias = ge_spotShadowParams.w * (1.0 - abs(dot(n, lightToFragN)));
    vec3 biasedPos = posWS + n * normalBias;

    vec4 shadowClip = ge_spotShadowVP * vec4(biasedPos, 1.0);
    if (shadowClip.w <= 1e-6)
        return 1.0;

    vec3 shadowNDC = shadowClip.xyz / shadowClip.w;
    vec2 shadowUV = shadowNDC.xy * 0.5 + 0.5;
    shadowUV.y = 1.0 - shadowUV.y;
    float refDepth = shadowNDC.z + ge_spotShadowParams.z;

    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0 ||
        refDepth < 0.0 || refDepth > 1.0)
        return 1.0;

    vec2 sc = GE_SpotShadowRotation(shadowUV);
    float shadow = 0.0;
    const int taps = 12;
    for (int i = 0; i < taps; ++i)
    {
        // Constant UV radius: the world blur then tracks receiver distance (the
        // projection is perspective) but not the map's resolution.
        vec2 offset = GE_VogelDisk(i, taps, sc) * kLocalFilterRadiusUV;
        shadow += GE_SHADOW_TAP(ge_spotShadowMap, vec3(shadowUV + offset, refDepth));
    }
    return shadow / float(taps);
}

int GE_PointShadowFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z)
        return dir.x >= 0.0 ? 0 : 1;
    if (a.y >= a.x && a.y >= a.z)
        return dir.y >= 0.0 ? 2 : 3;
    return dir.z >= 0.0 ? 4 : 5;
}

vec2 GE_PointShadowRotation(vec2 shadowUV, float face, float tileRes)
{
    // Per-texel Vogel rotation seed. tileRes (this slot's tile resolution) sets the
    // grid frequency so the noise scales with the tile, not a global resolution.
    vec2 shadowTexel = floor(clamp(shadowUV, vec2(0.0), vec2(0.999999)) * max(tileRes, 1.0));
    shadowTexel += vec2(face * 17.0, face * 31.0);
    float angle = fract(52.9829189 * fract(dot(shadowTexel, vec2(0.06711056, 0.00583715)))) * 6.2831853;
    return vec2(cos(angle), sin(angle));
}

float GE_SamplePointShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    if (lp.meta.y == 0u)
        return 1.0; // light casts no shadow
    int slot = lp.shadowSlots.x;
    // Out of range (unshadowed, or the fallback 1-slot buffer is bound because this
    // view published no atlas this frame): lit. .length() reflects the bound range.
    if (slot < 0 || slot >= int(ge_pointShadowSlots.length()))
        return 1.0;
    PointShadowSlot ps = ge_pointShadowSlots[slot];
    if (ps.params.x < 0.5)
        return 1.0; // slot disabled (defensive)

    float farPlane = ps.params2.z;
    vec3 lightToFrag = posWS - lp.posRange.xyz;
    float dist = length(lightToFrag);
    if (dist <= 1e-6 || dist > farPlane)
        return 1.0;

    vec3 lightToFragN = lightToFrag / dist;
    vec3 n = normalize(normalWS);
    float normalBias = ps.params.w * (1.0 - abs(dot(n, lightToFragN)));
    vec3 biasedPos = posWS + n * normalBias;
    vec3 biasedLightToFrag = biasedPos - lp.posRange.xyz;
    float biasedDist = length(biasedLightToFrag);
    if (biasedDist <= 1e-6 || biasedDist > farPlane)
        return 1.0;

    vec3 sampleDir = biasedLightToFrag / biasedDist;
    int face = GE_PointShadowFace(sampleDir);
    vec4 shadowClip = ps.faceVP[face] * vec4(biasedPos, 1.0);
    if (shadowClip.w <= 1e-6)
        return 1.0;

    vec3 shadowNDC = shadowClip.xyz / shadowClip.w;
    vec2 faceUV = shadowNDC.xy * 0.5 + 0.5;
    faceUV.y = 1.0 - faceUV.y; // negative-viewport Y-flip, tile-local (tile at layer origin)
    float refDepth = shadowNDC.z + ps.params.z;

    if (faceUV.x < 0.0 || faceUV.x > 1.0 ||
        faceUV.y < 0.0 || faceUV.y > 1.0 ||
        refDepth < 0.0 || refDepth > 1.0)
        return 1.0;

    // The tile occupies the [0, tileScale]^2 sub-rect of its layer at origin; a full
    // (High/Inherit) tile has tileScale 1.0 and this reduces to the full-layer path.
    float tileRes = max(ps.params2.x, 1.0);
    float tileScale = ps.params2.w;
    float atlasRes = tileRes / max(tileScale, 1e-6); // == kPointAtlasTileResolution
    float halfAtlasTexel = 0.5 / atlasRes;
    vec2 subLo = vec2(halfAtlasTexel);
    vec2 subHi = vec2(tileScale - halfAtlasTexel);
    float layer = ps.params.y + float(face); // baseLayer + face

    vec2 sc = GE_PointShadowRotation(faceUV, float(face), tileRes);
    float shadow = 0.0;
    const int taps = 12;
    for (int i = 0; i < taps; ++i)
    {
        // Constant radius in FACE UV, which is already tile-normalized — the
        // atlas mapping below scales it by tileScale, so no separate
        // tile-fraction correction is needed. Being tile-texel-relative was the
        // defect: tileRes moves at runtime with the coverage tier, so the blur
        // changed size mid-scene as a light dollied across a tier boundary.
        vec2 offset = GE_VogelDisk(i, taps, sc) * kLocalFilterRadiusUV;
        // Map the tap into the tile sub-rect and clamp (+half-texel gutter) so its
        // PCF comparison footprint can never bleed into the cleared-far border.
        vec2 atlasUV = clamp((faceUV + offset) * tileScale, subLo, subHi);
        shadow += GE_SHADOW_TAP(ge_pointShadowMap, vec4(atlasUV, layer, refDepth));
    }
    return shadow / float(taps);
}
#else
float GE_SampleAreaShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    return 1.0;
}

float GE_SampleSpotShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    return 1.0;
}

float GE_SamplePointShadow(GE_LightPacked lp, uint lightIndex, vec3 posWS, vec3 normalWS)
{
    return 1.0;
}
#endif

// Determine which cluster the fragment belongs to.
// `screenPos` is gl_FragCoord.xy; `linearDepth` is the fragment's linear (view-space) depth.
uvec3 GE_GetClusterIndex(vec2 screenPos, float linearDepth)
{
    uint tileX = ge_clusterParams0.x;
    uint tileY = ge_clusterParams0.y;
    uint depthSlices = max(ge_clusterParams0.z, 1u);

    // Read cluster grid dimensions from the header written by the cull pass.
    uint clustersX = max(ge_clusterHeader.x, 1u);
    uint clustersY = max(ge_clusterHeader.y, 1u);

    uint cx = uint(screenPos.x) / max(tileX, 1u);
    uint cy = uint(screenPos.y) / max(tileY, 1u);
    cx = min(cx, clustersX - 1u);
    cy = min(cy, clustersY - 1u);

    // Logarithmic depth slice (matches the cull shader).
    float zNear = max(ge_nearFar.x, 1e-4);
    float zFar = max(ge_nearFar.y, zNear + 1.0);
    float invLogRatio = ge_nearFar.z; // precomputed 1/log(far/near)
    uint slice = uint(max(log(linearDepth / zNear) * invLogRatio * float(depthSlices), 0.0));
    slice = min(slice, depthSlices - 1u);

    return uvec3(cx, cy, slice);
}

// Accumulate clustered lighting contribution for the given surface.
// Returns the total lit color (diffuse + specular) from all lights in the cluster
// plus the out-of-band directional lights (from LightUBO): the shadowed primary
// and up to 3 additive unshadowed secondaries.
//
// As of Phase D step 1, directional lights are NOT in the cluster index list —
// they're shaded from LightUBO @ binding 6 (primary uLightDirWorld/uLightColorWorld
// + the uSecondaryDirs/uSecondaryColors array). LightBuffer @ binding 4 only
// contains point + spot lights.
vec3 GE_AccumulateClusteredLighting(
    SurfaceOutput so, // surface props (baseColor / metallic / roughness / normalWS)
    vec3 V,           // world-space view direction (surface -> camera)
    vec3 posWS,       // world-space position (local-light distance / attenuation)
    vec3 posRel,      // render-origin-relative position (directional cascade shadow, Earth-scale)
    vec2 screenPos,   // gl_FragCoord.xy
    float linearDepth)
{
    vec3 totalLight = vec3(0.0);

    // Hoist the per-pixel base multiscatter compensation out of the per-light loop (it depends
    // only on F0/NdotV/roughness). GE_EvaluateStandardPBR reads g_BaseMultiscatter for every light.
    g_BaseMultiscatter = GE_ComputeBaseMultiscatter(
        mix(GE_DielectricF0(so.specularWeight, so.specularColor, so.specularIor), so.baseColor, so.metallic),
        max(dot(so.normalWS, V), 0.0), so.roughness);

    // -- Out-of-band directional light (read from LightUBO declared in adapter_forward.glsl) ----
    // Light.uLightDirWorld.xyz is the world-space LIGHT-TO-SURFACE direction (e.g.
    // (-0.5, -1, -0.3) for a sun pointing down). We need the surface->light vector L,
    // so negate.
    {
        vec3 dirL = normalize(-Light.uLightDirWorld.xyz);
        float dirI = Light.uLightDirWorld.w;
        vec3 dirCol = Light.uLightColorWorld.rgb;
        float dirAtten = 1.0;
        vec3 dirTint = vec3(1.0); // glass transmittance between the light and this surface
#ifdef HAS_SHADOWS
        if (Light.uLightColorWorld.w > 0.5) // castsShadows packed in .w
        {
            // Directional cascade shadow projects the render-origin-relative
            // position through the rebased ge_shadowVP (Earth-scale precision).
            dirAtten *= GE_SampleShadow(posRel, so.normalWS, linearDepth, dirL, screenPos);
            dirTint = ge_lastShadowTint;
        }
#endif
#ifdef GE_PARALLAX_MARCH
        // The surface's own relief shadows the primary light as its cascade shadow does.
        dirAtten *= so.primaryLightOcclusion;
#endif
        // Gate on the light the sun delivers, not its intensity alone: a sky that hides the moon
        // turns the sun off through its colour.
        if (dirI * max(dirCol.r, max(dirCol.g, dirCol.b)) > 0.0)
        {
            vec3 brdf = GE_EvaluateSurfaceLight(so, V, dirL);
            // The front-lit BRDF is shadowed AND glass-tinted; subsurface transmission is NOT
            // shadowed (light through a thin object must survive the object's own back-shadow)
            // — added outside dirAtten/dirTint.
            totalLight += brdf * dirCol * dirTint * dirI * dirAtten
                        + GE_SubsurfaceTransmission(so, V, dirL) * dirCol * dirI;
#ifdef HAS_SHADOWS
            // Glass caustics: focused-light dapple where glass concentrates the directional light
            // (see GE_GlassCaustic — gated by presence + NdotL, tinted by the glass, x the shadow
            // factor so there is no caustic inside an opaque shadow, and returned through the
            // receiver's own diffuse albedo). Faked C2, additive.
            totalLight += GE_GlassCaustic(posWS, so.normalWS, dirL, ge_lastGlassPresence,
                                          dirTint, so.baseColor * (1.0 - so.metallic),
                                          dirCol, dirI, dirAtten);
#endif
        }
    }

    // -- Secondary directional lights (same BRDF, additive, UNSHADOWED) ----------------------
    // v1 semantic: cascades exist for the primary only, so a secondary can brighten but never
    // darken — no shadow term, no glass tint/caustic (both are shadow-map products). Colors
    // arrive premultiplied (color * intensity); count is 0 in single-directional scenes, so
    // this uniform branch costs nothing there.
    {
        int secondaryCount = int(Light.uSecondaryCount.x);
        for (int i = 0; i < secondaryCount; ++i)
        {
            vec3 secL = normalize(-Light.uSecondaryDirs[i].xyz);
            vec3 secCol = Light.uSecondaryColors[i].rgb;
            totalLight += GE_EvaluateSurfaceLight(so, V, secL) * secCol
                        + GE_SubsurfaceTransmission(so, V, secL) * secCol;
        }
    }

    // -- Point + spot + area lights from the cluster list ------------------------------------
    uint lightCount = ge_lightHeader.x;
    if (lightCount == 0u)
        return totalLight;

    uint depthSlices = max(ge_clusterParams0.z, 1u);
    uint maxLightsPerCluster = max(ge_clusterParams0.w, 1u);
    uint clustersX = max(ge_clusterHeader.x, 1u);
    uint clustersY = max(ge_clusterHeader.y, 1u);

    uvec3 clusterIdx = GE_GetClusterIndex(screenPos, linearDepth);

    // 3D clusterId groups by slice (slice 0 tiles, then slice 1 tiles, ...).
    // Matches clustered_light_cull.comp's clusterId calculation.
    uint clusterId = clusterIdx.x + clusterIdx.y * clustersX
                     + clusterIdx.z * (clustersX * clustersY);
    uvec2 clusterData = ge_clusters[clusterId];
    uint firstIndex = clusterData.x;
    uint clusterLightCount = min(clusterData.y, maxLightsPerCluster);

    for (uint i = 0u; i < clusterLightCount; ++i)
    {
        uint lightIdx = ge_lightIndices[firstIndex + i];
        if (lightIdx >= lightCount)
            continue;

        GE_LightPacked lp = ge_lights[lightIdx];
        uint lightType = lp.meta.x;
        if (lp.meta.w == 0u)
            continue;
        vec3 lightColor = lp.colorAreaWidth.rgb * lp.dirIntensity.w; // color * intensity

        if (lightType == 4u)
        {
            // Subsurface transmission is deferred for area lights (would need per-sample eval);
            // the directional + point/spot paths add it outside the shadow multiply.
            float areaShadow = GE_SampleAreaShadow(lp, lightIdx, posWS, so.normalWS);
            totalLight += areaShadow * GE_EvaluateAreaLightFinite(lp, lightColor, so, V, posWS);
            continue;
        }

        vec3 L;
        float attenuation = 1.0;

        // Directional lights are shaded out-of-band above; defensively skip any
        // stragglers in case the upload path is mid-transition.
        if (lightType == 0u) continue;

        vec3 lightPos = lp.posRange.xyz;

        // Point, spot, or approximated area light.
        vec3 lightToFrag = posWS - lightPos;
        float dist = length(lightToFrag);
        float range = max(lp.posRange.w, 0.001);

        if (dist > range)
            continue;

        // light-to-surface direction (unit); L (surface-to-light) is its negation.
        vec3 lightToFragN = lightToFrag / max(dist, 1e-6);
        L = -lightToFragN;

        // Distance attenuation with imported FBX decay metadata.
        float distNorm = dist / range;
        attenuation = GE_LocalLightAttenuation(dist, range, max(lp.areaParams.z, 0.0), lp.areaUp.w);

        // Spot cone attenuation (type 2). Cosines of inner/outer angles are
        // packed by LightUploadNode into areaParams.w / spotParams.x.
        if (lightType == 2u)
        {
            // dirIntensity.xyz is already unit-length by upload convention
            // (RenderExtractionSystem normalizes the rotated forward axis), so
            // skip the per-pixel normalize.
            vec3 spotDir = lp.dirIntensity.xyz;
            float cosAngle = dot(lightToFragN, spotDir);
            float cosInner = lp.areaParams.w;
            float cosOuter = lp.spotParams.x;
            attenuation *= clamp((cosAngle - cosOuter) / max(cosInner - cosOuter, 1e-4), 0.0, 1.0);
        }

        float localShadow = 1.0;
#ifdef HAS_SHADOWS
        if (lightType == 1u)
            localShadow = GE_SamplePointShadow(lp, lightIdx, posWS, so.normalWS);
        else if (lightType == 2u)
            localShadow = GE_SampleSpotShadow(lp, lightIdx, posWS, so.normalWS);
#endif

        // Point and spot lights shadow the front-lit response while transmission
        // remains unshadowed.
        vec3 brdf = GE_EvaluateSurfaceLight(so, V, L);
        totalLight += (brdf * localShadow + GE_SubsurfaceTransmission(so, V, L)) * lightColor * attenuation;
    }

    return totalLight;
}

#endif // GE_CLUSTERED_LIGHTING_GLSL
