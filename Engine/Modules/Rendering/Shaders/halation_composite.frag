#version 450
#include "Includes/halation_blur.glsl"
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uHDR;
layout(set=0,binding=1) uniform sampler2D uHighlights;
layout(push_constant) uniform PC {
    float halationRadius;
    float halationIntensity;
    float halationTintR;
    float halationTintG;
    float halationTintB;
} pc;
void main()
{
    vec4 scene = texture(uHDR, vUV);
    vec3 halo = HalationBlur(uHighlights, vUV, vec2(0, 1), pc.halationRadius);
    vec3 tint = max(vec3(pc.halationTintR, pc.halationTintG, pc.halationTintB), 0.0);
    oColor = vec4(scene.rgb + halo * tint * max(pc.halationIntensity, 0.0), scene.a);
}
