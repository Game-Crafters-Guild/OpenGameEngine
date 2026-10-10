#version 450

#include "Includes/compat_profile.glsl"

// The compatibility cook uses explicit LOD for taps in non-uniform control
// flow, where WGSL cannot evaluate implicit derivatives. GE_TAP_LOD0 retains
// the native sampling form for other cooks.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 oColor;
layout(set = 0, binding = 0) uniform sampler2D uSceneColor;
layout(set = 0, binding = 1) uniform sampler2D uDepth;

layout(set = 0, binding = 2, std140) uniform ViewParamsBlock
{
#define GE_VP_MAT4(name) mat4 name;
#define GE_VP_VEC4(name) vec4 name;
#define GE_VIEWPARAMS_CORE_ONLY 1
#include "Includes/view_params_fields.glsl"
#undef GE_VIEWPARAMS_CORE_ONLY
#undef GE_VP_VEC4
#undef GE_VP_MAT4
} ViewParams;

#include "Includes/reverse_z.glsl"

layout(push_constant) uniform ChromaticAberrationPC
{
    float chromaticAberrationIntensity;
    float chromaticAberrationStartOffset;
    float chromaticAberrationSaturation;
    float chromaticAberrationLongitudinal;
    float chromaticAberrationComa;
    float dofFocusDistance;
} pc;

float Luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

float DefocusChannel(vec2 uv, vec2 radius, int channel)
{
    // A center-weighted disk avoids the four discrete diagonal copies produced
    // by the old ring kernel on small or distant high-contrast geometry.
    const float d = 0.70710678;
    const vec2 offsets[8] = vec2[](
        vec2( 0.45,  0.00), vec2(-0.45,  0.00),
        vec2( 0.00,  0.45), vec2( 0.00, -0.45),
        vec2( d,  d), vec2(-d,  d),
        vec2( d, -d), vec2(-d, -d));

    float value = GE_TAP_LOD0(uSceneColor, clamp(uv, vec2(0.0), vec2(1.0)))[channel] * 0.20;
    for (int i = 0; i < 4; ++i)
        value += GE_TAP_LOD0(uSceneColor, clamp(uv + offsets[i] * radius,
                                           vec2(0.0), vec2(1.0)))[channel] * 0.12;
    for (int i = 4; i < 8; ++i)
        value += GE_TAP_LOD0(uSceneColor, clamp(uv + offsets[i] * radius,
                                           vec2(0.0), vec2(1.0)))[channel] * 0.08;
    return value;
}

float AxialDepthWeight()
{
    ivec2 depthSize = textureSize(uDepth, 0);
    ivec2 pixel = ivec2(clamp(gl_FragCoord.xy, vec2(0.0), vec2(depthSize) - vec2(1.0)));
    float rawDepth = texelFetch(uDepth, pixel, 0).r;
    if (rawDepth <= 1e-6)
        return 0.0;

    float linearDepth = LinearizeReverseZ(rawDepth);
    float focusDistance = max(pc.dofFocusDistance, 0.01);
    float relativeDefocus = abs(linearDepth - focusDistance) /
                            max(max(linearDepth, focusDistance), 0.01);
    return smoothstep(0.02, 0.35, relativeDefocus);
}

void main()
{
    vec4 src = GE_TAP_LOD0(uSceneColor, vUV);
    vec2 texel = 1.0 / vec2(textureSize(uSceneColor, 0));
    float lateral = max(pc.chromaticAberrationIntensity, 0.0);
    float axial = max(pc.chromaticAberrationLongitudinal, 0.0);
    float coma = max(pc.chromaticAberrationComa, 0.0);

    vec2 centered = vUV - 0.5;
    float aspect = float(textureSize(uSceneColor, 0).x) /
                   max(float(textureSize(uSceneColor, 0).y), 1.0);
    vec2 radial = centered * vec2(aspect, 1.0);
    float radius01 = length(radial) / max(length(vec2(0.5 * aspect, 0.5)), 1e-5);
    float edge = smoothstep(clamp(pc.chromaticAberrationStartOffset, 0.0, 0.9999), 1.0, radius01);
    // radial is in pixel-aspect space; using centered here bends the effect
    // toward the wrong angle on non-square render targets.
    vec2 direction = length(radial) > 1e-6 ? normalize(radial) : vec2(0.0);
    vec3 rgb = src.rgb;

    if (lateral > 1e-5 || axial > 1e-5)
    {
        // Lateral is exposed as total red-to-blue separation, so each channel
        // moves half that distance in the opposite direction.
        vec2 offset = direction * texel * (0.5 * lateral) * edge * edge;
        vec2 rUv = clamp(vUV + offset, vec2(0.0), vec2(1.0));
        vec2 bUv = clamp(vUV - offset, vec2(0.0), vec2(1.0));
        // Keep the common lateral-only path at its original cost: depth and
        // disk samples are only needed when longitudinal CA is enabled.
        float axialRadius = axial > 1e-5 ? axial * AxialDepthWeight() : 0.0;
        vec3 shifted = axialRadius > 0.01
            ? vec3(DefocusChannel(rUv, texel * axialRadius, 0), src.g,
                   DefocusChannel(bUv, texel * axialRadius * 0.6, 2))
            : vec3(GE_TAP_LOD0(uSceneColor, rUv).r, src.g, GE_TAP_LOD0(uSceneColor, bUv).b);
        vec3 fringe = shifted - src.rgb;
        rgb = src.rgb + mix(vec3(Luma(fringe)), fringe,
                            clamp(pc.chromaticAberrationSaturation, 0.0, 2.0));
    }

    if (coma > 1e-5 && edge > 1e-4)
    {
        vec3 tail = vec3(0.0);
        float weightSum = 0.0;
        float tailLength = coma * edge * edge;
        // Keep adjacent copies within four pixels so small highlights form a
        // continuous trail instead of exposing the discrete integration taps.
        // Short tails retain the inexpensive six-tap path; only long, strongly
        // off-axis tails scale up to the sixteen-tap filter.
        int sampleCount = int(clamp(ceil(tailLength * 0.25), 6.0, 16.0));
        for (int i = 0; i < 16; ++i)
        {
            if (i >= sampleCount)
                break;
            // Sampling at bin centers avoids hard replicas at both ends of
            // the filtered interval.
            float t = (float(i) + 0.5) / float(sampleCount);
            float w = (1.0 - t) * (1.0 - t);
            vec3 s = GE_TAP_LOD0(uSceneColor, clamp(vUV - direction * texel * (tailLength * t),
                                                vec2(0.0), vec2(1.0))).rgb;
            tail += s * smoothstep(0.6, 1.0, Luma(s)) * w;
            weightSum += w;
        }
        tail /= max(weightSum, 1e-4);
        rgb = 1.0 - (1.0 - rgb) * (1.0 - clamp(tail, vec3(0.0), vec3(1.0)));
    }
    oColor = vec4(max(rgb, vec3(0.0)), src.a);
}
