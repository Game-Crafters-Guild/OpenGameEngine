#version 450
#include "Includes/exposed_brightness.glsl"
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uHDR;
layout(set=0,binding=1,std430) readonly buffer ExposureHistory { float exposureScale; uint valid; } uExposure;
layout(push_constant) uniform PC { float exposure; int useAutoExposure; } pc;

vec3 Highlight(ivec2 p, float scale)
{
    vec3 c = texelFetch(uHDR, clamp(p, ivec2(0), textureSize(uHDR, 0) - 1), 0).rgb;
    if (any(equal(floatBitsToUint(c) & uvec3(0x7f800000u), uvec3(0x7f800000u)))) return vec3(0);
    c = clamp(c, 0.0, 65000.0);
    float brightness = GE_ExposedMaxChannel(c, scale);
    // Fixed display-white threshold: film halation does not borrow bloom's
    // threshold, intensity, tint, depth veil or scattering source.
    float soft = clamp((brightness - 0.9) / 0.2, 0.0, 1.0);
    float excess = min(max(brightness - 1.0, soft * soft * 0.1), 100.0);
    return c * (excess / max(brightness, 1e-6));
}

void main()
{
    float scale = GE_ResolveExposureScale(pc.exposure, pc.useAutoExposure, uExposure.exposureScale);
    ivec2 p = ivec2(floor(vUV * vec2(textureSize(uHDR, 0)) - 0.5));
    // Extract each original texel BEFORE reduction, so small highlights do not
    // disappear merely because the half-resolution footprint dilutes them.
    oColor = vec4((Highlight(p, scale) + Highlight(p + ivec2(1, 0), scale)
                + Highlight(p + ivec2(0, 1), scale) + Highlight(p + ivec2(1), scale)) * 0.25, 0.0);
}
