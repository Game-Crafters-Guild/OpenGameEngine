// Local shadow inputs match SpotShadowDataGPU / PointShadowSlotGPU. Fog has
// no surface normal: only the explicit fog bias moves the sample toward light.
layout(set = 0, binding = 17, std140) uniform FogSpotShadowData
{
    mat4 fog_spotShadowVP;
    vec4 fog_spotShadowParams;
    vec4 fog_spotShadowParams2;
};
layout(set = 0, binding = 18) uniform sampler2DShadow uFogSpotShadow;

struct FogPointShadowSlot
{
    mat4 faceVP[6];
    vec4 params;
    vec4 params2;
};
layout(set = 0, binding = 19, std430) readonly buffer FogPointShadowData
{
    FogPointShadowSlot fog_pointShadowSlots[];
};
layout(set = 0, binding = 20) uniform sampler2DArrayShadow uFogPointShadow;

int FogPointShadowFace(vec3 dir)
{
    vec3 a = abs(dir);
    if (a.x >= a.y && a.x >= a.z)
        return dir.x >= 0.0 ? 0 : 1;
    if (a.y >= a.x && a.y >= a.z)
        return dir.y >= 0.0 ? 2 : 3;
    return dir.z >= 0.0 ? 4 : 5;
}

bool FogProjectLocalShadow(mat4 vp, vec3 worldPos, float depthBias, out vec3 uvDepth)
{
    vec4 clip = vp * vec4(worldPos, 1.0);
    if (clip.w <= 1.0e-6)
        return false;
    vec3 ndc = clip.xyz / clip.w;
    uvDepth = vec3(ndc.xy * vec2(0.5, -0.5) + 0.5, ndc.z + depthBias);
    return all(greaterThanEqual(uvDepth, vec3(0.0))) &&
           all(lessThanEqual(uvDepth, vec3(1.0)));
}

float FogLocalShadow(FogLocalLightPacked light, uint lightIndex, vec3 worldPos)
{
    if (light.meta.y == 0u)
        return 1.0;
    vec3 toLight = light.posRange.xyz - worldPos;
    float distanceToLight = length(toLight);
    if (distanceToLight <= 1.0e-6)
        return 1.0;
    vec3 biasedPos = worldPos + toLight / distanceToLight * fog.uFogExtraParams.y;
    const vec2 offsets[5] = vec2[](vec2(0.0), vec2(-1.0, -1.0),
        vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));

    if (light.meta.x == 2u)
    {
        if (fog_spotShadowParams.x < 0.5 ||
            abs(float(lightIndex) - fog_spotShadowParams.y) > 0.5 ||
            distanceToLight > fog_spotShadowParams2.z)
            return 1.0;
        vec3 uvDepth;
        if (!FogProjectLocalShadow(fog_spotShadowVP, biasedPos, fog_spotShadowParams.z, uvDepth))
            return 1.0;
        vec2 texel = 1.0 / vec2(textureSize(uFogSpotShadow, 0));
        float visibility = 0.0;
        for (int i = 0; i < 5; ++i)
            visibility += texture(uFogSpotShadow, vec3(uvDepth.xy + offsets[i] * texel, uvDepth.z));
        return visibility * 0.2;
    }

    if (light.meta.x == 1u)
    {
        int slot = light.shadowSlots.x;
        if (slot < 0 || slot >= fog_pointShadowSlots.length())
            return 1.0;
        FogPointShadowSlot shadow = fog_pointShadowSlots[slot];
        if (shadow.params.x < 0.5 || distanceToLight > shadow.params2.z)
            return 1.0;
        int face = FogPointShadowFace(biasedPos - light.posRange.xyz);
        vec3 uvDepth;
        if (!FogProjectLocalShadow(shadow.faceVP[face], biasedPos, shadow.params.z, uvDepth))
            return 1.0;
        // Each atlas layer may hold a smaller tile at its origin. Clamp every
        // comparison footprint inside that tile, including reduced quality tiers.
        float tileScale = shadow.params2.w;
        vec2 texel = 1.0 / vec2(textureSize(uFogPointShadow, 0).xy);
        vec2 lo = texel * 0.5;
        vec2 hi = max(vec2(tileScale) - lo, lo);
        float layer = shadow.params.y + float(face);
        float visibility = 0.0;
        for (int i = 0; i < 5; ++i)
        {
            vec2 uv = clamp(uvDepth.xy * tileScale + offsets[i] * texel, lo, hi);
            visibility += texture(uFogPointShadow, vec4(uv, layer, uvDepth.z));
        }
        return visibility * 0.2;
    }
    return 1.0;
}
