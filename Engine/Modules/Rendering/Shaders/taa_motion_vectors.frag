#version 450

// TAA movers motion-vector pass, fragment stage: viewport-UV-space motion
// delta for the visible mover surface. The target is cleared to a sentinel
// (see taa_resolve.comp kMVSentinel); any written texel means "an exact
// per-instance MV exists here".

#include "Includes/screen_position.glsl"

layout(location = 0) in vec4 vCurrClip;
layout(location = 1) in vec4 vPrevClip;

layout(location = 0) out vec4 outMV;

void main()
{
    if (vCurrClip.w <= 1e-6 || vPrevClip.w <= 1e-6)
    {
        // No usable previous surface: retain the unwritten sentinel.
        outMV = vec4(100.0, 100.0, 0.0, 0.0);
        return;
    }
    vec2 currUv = GE_YUpNdcToViewportUV(vCurrClip.xy / vCurrClip.w);
    vec2 prevUv = GE_YUpNdcToViewportUV(vPrevClip.xy / vPrevClip.w);
    outMV = vec4(currUv - prevUv, vPrevClip.z / vPrevClip.w, 1.0);
}
