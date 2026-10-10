#version 450

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

// Resolved single-sample R32F depth (View.DepthResolved).
layout(set = 0, binding = 0) uniform sampler2D DepthTex;

// ClusterBuffer written by clustered_light_cull.comp.
//   header @ [0]            = (clustersX, clustersY, depthSlices, maxLightsPerCluster)
//   clusters[3DclusterId]   = uvec2(firstIndex, count)
//   where 3DclusterId = tx + ty*cX + sli*cX*cY
layout(set = 0, binding = 1, std430) readonly buffer ClusterBufferBlock
{
    uvec4 header;
    uvec2 clusters[];
} ClusterBuffer;

// LightIndexBuffer written by clustered_light_cull.comp (uint indices).
layout(set = 0, binding = 2, std430) readonly buffer LightIndexBufferBlock
{
    uint indices[];
} LightIndexBuffer;

struct LightPacked
{
#include "Includes/light_packed_fields.glsl"
};
layout(set = 0, binding = 3, std430) readonly buffer LightBufferBlock
{
    uvec4 header;
    LightPacked lights[];
} LightBuffer;

layout(set = 0, binding = 4, std140) uniform ClusterParamsBlock
{
    uvec4 params0; // tileX, tileY, depthSlices, maxLightsPerCluster
} ClusterParams;

layout(set = 0, binding = 5, std140) uniform ViewParamsBlock
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

float ViewZFromDepth_ReverseZ_LH(float depth, float zNear, float zFar)
{
    // For MakePerspectiveLH_ZO_ReverseZ:
    //   depth = -n/(f-n) + (n*f/(f-n)) * (1/z)
    //   => z = n*f / (depth*(f-n) + n)
    // Guard against depth == 0 (cleared / sky / unwritten) to avoid div-by-zero;
    // return a very-large finite value so downstream cluster slicing won't NaN.
    const float denom = max(depth * (zFar - zNear) + zNear, 1e-6);
    return (zNear * zFar) / denom;
}

void main()
{
    const uint clustersX = max(ClusterBuffer.header.x, 1u);
    const uint clustersY = max(ClusterBuffer.header.y, 1u);
    const uint depthSlices = max(ClusterBuffer.header.z, 1u);
    const uint maxLightsPerCluster = max(ClusterBuffer.header.w, 1u);

    const uvec2 tileSize = max(uvec2(ClusterParams.params0.xy), uvec2(1, 1));
    const vec2 renderSize = vec2(float(clustersX * tileSize.x), float(clustersY * tileSize.y));

    const vec2 fragXY = gl_FragCoord.xy;
    const uvec2 pix = uvec2(clamp(fragXY, vec2(0.0), renderSize - vec2(1.0)));
    const float depth = texelFetch(DepthTex, ivec2(pix), 0).r;

#if defined(GE_COMPAT_PROFILE)
    // WGSL requires derivative ops to sit in uniform control flow, so compat
    // reconstructs the view-space position and its derivatives before any
    // early-out. The values are quad-local either way, so surviving pixels see
    // identical results; sky and out-of-bounds pixels compute garbage here and
    // discard it below.
    const vec2 ndc = (fragXY / renderSize) * 2.0 - 1.0;
    vec4 pV = ViewParams.ge_invProj * vec4(ndc.x, ndc.y, depth, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    const vec3 posV = pV.xyz;
    vec3 nV = normalize(cross(dFdx(posV), dFdy(posV)));
#endif

    // Reverse-Z: empty/sky pixels are at depth == 0.0.
    if (depth <= 0.000001)
    {
        oColor = vec4(0.0);
        return;
    }

    const uvec2 tile = uvec2(pix) / tileSize;
    if (tile.x >= clustersX || tile.y >= clustersY)
    {
        oColor = vec4(0.0);
        return;
    }

    const float zNear = max(ViewParams.ge_nearFar.x, 1e-4);
    float zFar = ViewParams.ge_nearFar.y;
    if (zFar <= zNear + 1e-4) zFar = zNear + 1000.0;

    const float viewZ = ViewZFromDepth_ReverseZ_LH(depth, zNear, zFar);
    const float invLogRatio = ViewParams.ge_nearFar.z;
    const float t = clamp(log(viewZ / zNear) * invLogRatio, 0.0, 0.999999);
    const uint slice = uint(floor(t * float(depthSlices)));

    // V2 schema: 3D clusterId groups by slice (sli*cX*cY + ty*cX + tx).
    const uint clusterId = tile.x + tile.y * clustersX + slice * (clustersX * clustersY);
    const uvec2 c = ClusterBuffer.clusters[clusterId];
    const uint base = c.x;
    const uint count = min(c.y, maxLightsPerCluster);

#if !defined(GE_COMPAT_PROFILE)
    // Reconstruct view-space position from invProj.
    const vec2 ndc = (fragXY / renderSize) * 2.0 - 1.0;
    vec4 pV = ViewParams.ge_invProj * vec4(ndc.x, ndc.y, depth, 1.0);
    pV.xyz /= max(pV.w, 1e-6);
    const vec3 posV = pV.xyz;

    // Approximate view-space normal from derivatives.
    vec3 nV = normalize(cross(dFdx(posV), dFdy(posV)));
#endif
    if (dot(nV, -normalize(posV)) < 0.0) nV = -nV;

    vec3 accum = vec3(0.0);
    for (uint i = 0u; i < count; ++i)
    {
        const uint li = LightIndexBuffer.indices[base + i];
        if (li >= LightBuffer.header.x)
            continue;

        const LightPacked L = LightBuffer.lights[li];
        if (L.meta.w == 0u)
            continue;
        const vec3 color = L.colorAreaWidth.rgb;
        const float intensity = L.dirIntensity.w;

        const uint lightType = L.meta.x;
        if (lightType == 0u)
        {
            // Directional.
            const vec3 dirWS = normalize(L.dirIntensity.xyz);
            const vec3 dirV = normalize((ViewParams.ge_view * vec4(dirWS, 0.0)).xyz);
            const vec3 ldir = normalize(-dirV); // surface -> light
            const float ndotl = max(dot(nV, ldir), 0.0);
            accum += color * (intensity * ndotl);
        }
        else
        {
            const vec3 lightPosWS = L.posRange.xyz;
            const float range = max(L.posRange.w, 0.001);
            const vec3 lightPosV = (ViewParams.ge_view * vec4(lightPosWS, 1.0)).xyz;

            const vec3 dV = lightPosV - posV;
            const float dist = length(dV);
            const vec3 ldir = dV / max(dist, 1e-6); // surface -> light
            const float ndotl = max(dot(nV, ldir), 0.0);
            const float atten = Saturate(1.0 - (dist / range));

            float spot = 1.0;
            if (lightType == 2u)
            {
                const vec3 dirWS = normalize(L.dirIntensity.xyz);
                const vec3 dirV = normalize((ViewParams.ge_view * vec4(dirWS, 0.0)).xyz);
                const float cosInner = L.areaParams.w;
                const float cosOuter = L.spotParams.x;
                const vec3 lightToP = normalize(-ldir);
                const float cd = dot(lightToP, normalize(dirV));
                spot = smoothstep(cosOuter, cosInner, cd);
            }

            accum += color * (intensity * ndotl * atten * atten * spot);
        }
    }

    // Pure additive contribution.
    oColor = vec4(accum, 1.0);
}
