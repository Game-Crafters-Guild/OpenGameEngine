#version 450

// Post pass over single-mip targets: lod-0 taps are exact and legal in
// non-uniform flow under WGSL.
#include "Includes/compat_profile.glsl"

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
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

#include "Includes/reverse_z.glsl"
#include "Includes/hdr_sanitize.glsl"

layout(push_constant) uniform FastBlurPC {
    float fastBlurIntensity;
    float fastBlurFocusDistance;
    float fastBlurFocusRange;
    float fastBlurMaxRadius;
    int fastBlurNearBlur;
} pc;


float CircleOfConfusion(float linearDepth)
{
    float focus = max(pc.fastBlurFocusDistance, 0.01);
    float range = max(pc.fastBlurFocusRange, 0.01);
    float delta = linearDepth - focus;
    if (delta < 0.0 && pc.fastBlurNearBlur == 0)
        return 0.0;
    return clamp((abs(delta) - range * 0.5) / range, 0.0, 1.0);
}


vec3 SampleColor(vec2 uv)
{
    return Sanitize(GE_TAP_LOD0(uSceneColor, clamp(uv, vec2(0.0), vec2(1.0))).rgb);
}

void main()
{
    vec4 center = GE_TAP_LOD0(uSceneColor, vUV);
    vec3 centerRgb = Sanitize(center.rgb);
    float intensity = clamp(pc.fastBlurIntensity, 0.0, 1.0);
    float maxRadius = max(pc.fastBlurMaxRadius, 0.0);
    if (intensity <= 1e-5 || maxRadius <= 1e-5)
    {
        oColor = vec4(centerRgb, center.a);
        return;
    }

    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pix = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(depthSize) - vec2(1.0)));
    float rawDepth = texelFetch(uDepth, pix, 0).r;
    if (rawDepth <= 0.0)
    {
        oColor = vec4(centerRgb, center.a);
        return;
    }

    float linearDepth = LinearizeReverseZ(rawDepth);
    float coc = CircleOfConfusion(linearDepth) * intensity;
    float radiusPx = coc * maxRadius;
    if (radiusPx <= 0.25)
    {
        oColor = vec4(centerRgb, center.a);
        return;
    }

    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));
    vec2 r = texel * radiusPx;

    vec3 acc = centerRgb * 0.22;
    float weight = 0.22;

    const vec2 offsets[8] = vec2[](
        vec2( 1.000,  0.000),
        vec2(-1.000,  0.000),
        vec2( 0.000,  1.000),
        vec2( 0.000, -1.000),
        vec2( 0.707,  0.707),
        vec2(-0.707,  0.707),
        vec2( 0.707, -0.707),
        vec2(-0.707, -0.707)
    );

    for (int i = 0; i < 8; ++i)
    {
        float w = (i < 4) ? 0.105 : 0.09;
        acc += SampleColor(vUV + offsets[i] * r) * w;
        weight += w;
    }

    vec3 blurred = acc / max(weight, 1e-5);
    oColor = vec4(mix(centerRgb, blurred, coc), center.a);
}
