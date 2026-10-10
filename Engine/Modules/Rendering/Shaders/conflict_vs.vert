#version 450

// VS declares set0,binding0 as uniform buffer
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 proj;
} uPerFrame;

layout(location = 0) in vec3 inPos;
void main() {
    gl_Position = vec4(inPos, 1.0);
}

