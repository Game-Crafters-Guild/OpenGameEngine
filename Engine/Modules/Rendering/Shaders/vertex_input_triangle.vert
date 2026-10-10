#version 450

// Vertex input from vertex buffer
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;

// Output to fragment shader
layout(location = 0) out vec3 fragColor;

void main() {
    // Use actual vertex buffer data instead of hardcoded values
    gl_Position = vec4(inPosition, 1.0);
    fragColor = inColor;
}
