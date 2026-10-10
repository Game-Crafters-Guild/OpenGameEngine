// Forward+ lit variant of the minimal world debug shader.
// Consumes clustered light lists produced by clustered_light_cull.comp.
#version 450

layout(location = 0) in vec3 vColor;
layout(location = 1) in vec3 vPosWS;
layout(location = 0) out vec4 outColor;

// V2 ClusterBuffer schema: uvec4 header + uvec2 (firstIndex, count) per 3D clusterId.
layout(set = 0, binding = 0, std430) readonly buffer ClusterBufferBlock { uvec4 header; uvec2 clusters[]; } ClusterBuffer;
layout(set = 0, binding = 1, std430) readonly buffer LightIndexBufferBlock { uint indices[]; } LightIndexBuffer;
struct LightPacked
{
#include "Includes/light_packed_fields.glsl"
};
layout(set = 0, binding = 2, std430) readonly buffer LightBufferBlock { uvec4 header; LightPacked lights[]; } LightBuffer;
layout(set = 0, binding = 3, std140) uniform ClusterParamsBlock { uvec4 params0; } ClusterParams; // tileX, tileY, depthSlices, maxLightsPerCluster
layout(set = 0, binding = 4, std140) uniform ViewParamsBlock
{
    // 160 B core prefix of the shared ViewParams list.
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

float Saturate(float x) { return clamp(x, 0.0, 1.0); }

void main()
{
    const uint clustersX = max(ClusterBuffer.header.x, 1u);
    const uint clustersY = max(ClusterBuffer.header.y, 1u);
    const uint depthSlices = max(ClusterBuffer.header.z, 1u);
    const uint maxLightsPerCluster = max(ClusterBuffer.header.w, 1u);
    const uint lightCount = LightBuffer.header.x;

    vec3 albedo = vColor;

#if defined(GE_COMPAT_PROFILE)
    // WGSL requires workgroup-uniform control flow around derivative ops, so
    // compat takes them before any early-out; dFdx/dFdy are quad-local either way.
    const vec3 posV = (ViewParams.ge_view * vec4(vPosWS, 1.0)).xyz;
    vec3 nV = normalize(cross(dFdx(posV), dFdy(posV)));
    if (dot(nV, -normalize(posV)) < 0.0) nV = -nV;
#endif

    if (lightCount == 0u)
    {
        outColor = vec4(albedo, 1.0);
        return;
    }

#if !defined(GE_COMPAT_PROFILE)
    const vec3 posV = (ViewParams.ge_view * vec4(vPosWS, 1.0)).xyz;
#endif
    const float zNear = max(ViewParams.ge_nearFar.x, 1e-4);
    float zFar = ViewParams.ge_nearFar.y;
    if (zFar <= zNear + 1e-4) zFar = zNear + 1000.0;

    const float invLogRatio = ViewParams.ge_nearFar.z;
    const float t = clamp(log(max(posV.z, zNear) / zNear) * invLogRatio, 0.0, 0.999999);
    const uint slice = uint(floor(t * float(depthSlices)));

    const uvec2 tileSize = max(uvec2(ClusterParams.params0.xy), uvec2(1, 1));
    const uvec2 tile = uvec2(gl_FragCoord.xy) / tileSize;
    if (tile.x >= clustersX || tile.y >= clustersY)
    {
        outColor = vec4(albedo, 1.0);
        return;
    }
    // V2 schema: 3D clusterId groups by slice.
    const uint clusterId = tile.x + tile.y * clustersX + slice * (clustersX * clustersY);
    const uvec2 c = ClusterBuffer.clusters[clusterId];
    const uint base = c.x;
    const uint count = min(c.y, maxLightsPerCluster);

#if !defined(GE_COMPAT_PROFILE)
    // View-space normal from geometry derivatives (works even without vertex normals).
    vec3 nV = normalize(cross(dFdx(posV), dFdy(posV)));
    // Ensure normal faces the camera.
    if (dot(nV, -normalize(posV)) < 0.0) nV = -nV;
#endif

    vec3 lightAccum = vec3(0.0);
    for (uint i = 0u; i < count; ++i)
    {
        const uint li = LightIndexBuffer.indices[base + i];
        if (li >= lightCount) continue;
        const LightPacked L = LightBuffer.lights[li];

        const uint lightType = L.meta.x;
        if (L.meta.w == 0u)
            continue;
        const vec3 color = L.colorAreaWidth.rgb;
        const float intensity = L.dirIntensity.w;

        if (lightType == 0u)
        {
            // Directional: directionWS stored in dirIntensity.xyz.
            const vec3 dirWS = normalize(L.dirIntensity.xyz);
            const vec3 dirV = normalize((ViewParams.ge_view * vec4(dirWS, 0.0)).xyz);
            const vec3 ldir = normalize(-dirV); // surface -> light
            const float ndotl = max(dot(nV, ldir), 0.0);
            lightAccum += color * (intensity * ndotl);
        }
        else
        {
            // Point: sphere attenuation.
            const vec3 lightPosWS = L.posRange.xyz;
            const float range = max(L.posRange.w, 0.001);
            const vec3 lightPosV = (ViewParams.ge_view * vec4(lightPosWS, 1.0)).xyz;

            const vec3 dV = lightPosV - posV;
            const float dist = length(dV);
            const vec3 ldir = dV / max(dist, 1e-6);
            const float ndotl = max(dot(nV, ldir), 0.0);
            const float atten = Saturate(1.0 - (dist / range));

            float spot = 1.0;
            if (lightType == 2u)
            {
                // Spot: dirIntensity.xyz is directionWS; meta.zw pack cos(inner/outer).
                const vec3 dirWS = normalize(L.dirIntensity.xyz);
                const vec3 dirV = normalize((ViewParams.ge_view * vec4(dirWS, 0.0)).xyz);
                const float cosInner = L.areaParams.w;
                const float cosOuter = L.spotParams.x;

                // light->surface direction in view space
                const vec3 lightToP = normalize(-ldir);
                const float cd = dot(lightToP, normalize(dirV));
                spot = smoothstep(cosOuter, cosInner, cd);
            }

            lightAccum += color * (intensity * ndotl * atten * atten * spot);
        }
    }

    const vec3 ambient = vec3(0.08);
    outColor = vec4(albedo * (ambient + lightAccum), 1.0);
}
