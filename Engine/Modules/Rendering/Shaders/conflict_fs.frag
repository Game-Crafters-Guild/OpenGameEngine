#version 450

// FS declares set0,binding0 as sampler2D (conflicts with VS uniform buffer)
layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = texture(uTex, vUV);
}

