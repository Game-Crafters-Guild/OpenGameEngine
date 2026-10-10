#version 450

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform PC {
    vec4 color;
    vec4 depthAndPad; // x = depth in [0,1]
} pc;

void main() {
    outColor = pc.color;
    gl_FragDepth = pc.depthAndPad.x;
}

