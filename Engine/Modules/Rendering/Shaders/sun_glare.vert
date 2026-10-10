#version 450

// Sun glare, SINGLE-SAMPLE scene depth. The whole vertex stage lives in the shared include;
// this file exists to declare the depth uniform's type and point the probe at it.

layout(set = 0, binding = 1) uniform sampler2D uSceneDepth;

#define GE_SUN_GLARE_PROBE(sunUV, radiusUV) \
    GE_SunGlareScreenVisibility(uSceneDepth, sunUV, radiusUV)

#include "Includes/sun_glare_vs.glsl"
