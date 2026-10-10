#version 450
#include "Includes/halation_blur.glsl"
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 oColor;
layout(set=0,binding=0) uniform sampler2D uHighlights;
layout(push_constant) uniform PC { float halationRadius; } pc;
void main()
{
    oColor = vec4(HalationBlur(uHighlights, vUV, vec2(1, 0), pc.halationRadius), 0.0);
}
