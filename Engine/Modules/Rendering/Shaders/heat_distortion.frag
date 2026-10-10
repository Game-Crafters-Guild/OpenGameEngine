#version 450

// Post passes sample single-mip render targets: lod-0 taps are exact and
// satisfy WGSL uniformity inside the effect branches.
#include "Includes/compat_profile.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
    // 240 B prefix of the shared ViewParams list (through viewProj, no proj).
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_OMIT_PROJ 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_OMIT_PROJ
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

layout(push_constant) uniform HeatDistortionPC
{
    float heatDistortionStrength;
    float heatDistortionSpeed;
    float heatDistortionScale;
    float heatDistortionMaskStrength;
    float heatDistortionDistanceStart;
    float heatDistortionDistanceEnd;
    float heatDistortionDirectionalFalloff;
    int heatDistortionUseAbsoluteY;
    float heatDistortionSoftness;
} pc;

float Saturate(float x)
{
    return clamp(x, 0.0, 1.0);
}

vec2 ViewportUvToNdc(vec2 uv)
{
    return vec2(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0);
}

float Hash(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float Noise(vec3 p)
{
    vec3 i = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = Hash(i + vec3(0.0, 0.0, 0.0));
    float n100 = Hash(i + vec3(1.0, 0.0, 0.0));
    float n010 = Hash(i + vec3(0.0, 1.0, 0.0));
    float n110 = Hash(i + vec3(1.0, 1.0, 0.0));
    float n001 = Hash(i + vec3(0.0, 0.0, 1.0));
    float n101 = Hash(i + vec3(1.0, 0.0, 1.0));
    float n011 = Hash(i + vec3(0.0, 1.0, 1.0));
    float n111 = Hash(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, f.x);
    float nx10 = mix(n010, n110, f.x);
    float nx01 = mix(n001, n101, f.x);
    float nx11 = mix(n011, n111, f.x);
    float nxy0 = mix(nx00, nx10, f.y);
    float nxy1 = mix(nx01, nx11, f.y);
    return mix(nxy0, nxy1, f.z);
}

float LinearizeReverseZ(float depth)
{
    float zNear = max(ViewParams.ge_nearFar.x, 1e-5);
    float zFar = max(ViewParams.ge_nearFar.y, zNear + 1.0);
    return (zNear * zFar) / max(depth * (zFar - zNear) + zNear, 1e-6);
}

vec3 ViewRay(vec2 uv, float rawDepth)
{
    vec4 p = ViewParams.ge_invProj * vec4(ViewportUvToNdc(uv), rawDepth, 1.0);
    vec3 viewPos = p.xyz / max(abs(p.w), 1e-6);
    return normalize(viewPos);
}

vec2 DistortionVector(vec2 uv)
{
    float time = ViewParams.ge_nearFar.w * pc.heatDistortionSpeed;
    float scale = max(pc.heatDistortionScale, 0.01);
    vec2 scroll = vec2(time * 0.07, -time * 2.65);
    vec2 p = uv * scale + scroll;
    float slowPhase = time * 0.11;

    vec2 primary = vec2(
        Noise(vec3(p, slowPhase)),
        Noise(vec3(p + vec2(17.3, 9.1), slowPhase + 6.7)));

    vec2 fineP = uv * (scale * 1.72) + vec2(-time * 0.04, -time * 3.85) + vec2(31.4, 12.8);
    vec2 fine = vec2(
        Noise(vec3(fineP, slowPhase + 11.2)),
        Noise(vec3(fineP + vec2(5.9, 23.6), slowPhase + 19.4)));

    vec2 plumeField = mix(primary, fine, 0.22);
    return (plumeField - vec2(0.5)) * 2.0;
}

vec3 SoftSample(vec2 uv, float radiusPx)
{
    vec2 texel = ViewParams.ge_screenSize.zw;
    vec2 r = texel * radiusPx;
    vec3 c = GE_TAP_LOD0(uSceneColor, uv).rgb * 0.36;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2( r.x, 0.0), vec2(0.0), vec2(1.0))).rgb * 0.11;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2(-r.x, 0.0), vec2(0.0), vec2(1.0))).rgb * 0.11;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2(0.0,  r.y), vec2(0.0), vec2(1.0))).rgb * 0.11;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2(0.0, -r.y), vec2(0.0), vec2(1.0))).rgb * 0.11;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2( r.x,  r.y) * 0.72, vec2(0.0), vec2(1.0))).rgb * 0.05;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2(-r.x,  r.y) * 0.72, vec2(0.0), vec2(1.0))).rgb * 0.05;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2( r.x, -r.y) * 0.72, vec2(0.0), vec2(1.0))).rgb * 0.05;
    c += GE_TAP_LOD0(uSceneColor, clamp(uv + vec2(-r.x, -r.y) * 0.72, vec2(0.0), vec2(1.0))).rgb * 0.05;
    return c;
}

void main()
{
    vec4 scene = GE_TAP_LOD0(uSceneColor, vUV);
    float strengthPx = max(pc.heatDistortionStrength, 0.0);
    if (strengthPx <= 1e-4 || pc.heatDistortionMaskStrength <= 1e-4)
    {
        oColor = scene;
        return;
    }

    float rawDepth = GE_TAP_LOD0(uDepth, vUV).r;
    float linearDepth = rawDepth <= 1e-6 ? ViewParams.ge_nearFar.y : LinearizeReverseZ(rawDepth);
    float depth01 = Saturate(linearDepth / max(ViewParams.ge_nearFar.y, 1.0));
    float depthMask = smoothstep(pc.heatDistortionDistanceStart,
                                 max(pc.heatDistortionDistanceEnd, pc.heatDistortionDistanceStart + 1e-4),
                                 depth01);

    vec3 viewDirVS = ViewRay(vUV, rawDepth <= 1e-6 ? 0.0 : rawDepth);
    vec3 viewDirWS = normalize(transpose(mat3(ViewParams.ge_view)) * viewDirVS);
    float y = pc.heatDistortionUseAbsoluteY != 0 ? abs(viewDirWS.y) : max(viewDirWS.y, 0.0);
    float viewMask = pow(Saturate(1.0 - y), max(pc.heatDistortionDirectionalFalloff, 0.0));
    float edgeMask = smoothstep(0.0, 0.03, vUV.x) * smoothstep(0.0, 0.03, vUV.y) *
                     smoothstep(0.0, 0.03, 1.0 - vUV.x) * smoothstep(0.0, 0.03, 1.0 - vUV.y);
    float mask = Saturate(depthMask * viewMask * edgeMask * pc.heatDistortionMaskStrength);
    if (mask <= 1e-4)
    {
        oColor = scene;
        return;
    }

    vec2 texel = ViewParams.ge_screenSize.zw;
    vec2 offset = DistortionVector(vUV) * strengthPx * mask * texel;
    vec2 warpedUv = clamp(vUV + offset, vec2(0.0), vec2(1.0));

    vec3 warped = GE_TAP_LOD0(uSceneColor, warpedUv).rgb;
    vec3 softened = SoftSample(warpedUv, max(pc.heatDistortionSoftness, 0.0) * (0.5 + mask));
    vec3 color = mix(warped, softened, Saturate(pc.heatDistortionSoftness * 0.32 * mask));
    oColor = vec4(mix(scene.rgb, color, mask), scene.a);
}
